#include "cellscene.hpp"

#include <osg/MatrixTransform>

#include <components/misc/cellgrid.hpp>
#include <components/misc/constants.hpp>

#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <components/debug/debuglog.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/rtx/lightbuilder.hpp>
#include <components/rtx/texturebuilder.hpp>
#include <components/sceneutil/lightcommon.hpp>
#include <components/sceneutil/lightutil.hpp>
#include <components/sceneutil/vismask.hpp>

namespace RtxTool
{
    namespace
    {
        osg::ref_ptr<osg::Group> readObjects(
            World& world, const ESM::Cell& cell, osg::Group& root, CellReport& report, bool liveProps);

        /// Calls `visit` for every cell in the region that `loaded` does not already name, and adds
        /// each one to it.
        ///
        /// An interior is its own region: it has no neighbours to have. An exterior square that runs
        /// off the edge of the world, or over open sea, simply finds fewer cells — a coastline is
        /// not an error.
        /// The square of cells this harness keeps loaded, around the cell the caller named.
        ///
        /// **Derived once, and the terrain is told this very grid.** `World::setActiveCellGrid`
        /// takes it, so which cells are standing and which square `Terrain::ObjectPaging` refuses
        /// to page are two views of one answer rather than two rules that can drift apart. The
        /// order the cells come out in is the game's, which matters to anything timing a camera
        /// across a boundary. `Misc::CellGrid` owns both.
        Misc::CellGrid gridAround(const ESM::Cell& centre)
        {
            return Misc::CellGrid(osg::Vec2i(centre.getGridX(), centre.getGridY()), Constants::CellGridRadius);
        }

        /// How a cell is keyed, and it has to be what the grid walk builds or nothing matches.
        std::string keyOf(const ESM::Cell& cell)
        {
            return cell.isExterior() ? std::to_string(cell.getGridX()) + ',' + std::to_string(cell.getGridY())
                                     : cell.mName;
        }

        void forEachNewCell(World& world, const ESM::Cell& centre, LoadedCells& loaded,
            const std::function<void(const ESM::Cell&)>& visit)
        {
            if (!centre.isExterior())
            {
                if (loaded.emplace(centre.mName, nullptr).second)
                    visit(centre);

                return;
            }

            std::vector<osg::Vec2i> square;
            gridAround(centre).listCells(square);

            for (const osg::Vec2i& position : square)
            {
                std::string spec = std::to_string(position.x()) + ',' + std::to_string(position.y());
                if (loaded.contains(spec))
                    continue;

                if (const ESM::Cell* cell = world.findCell(spec))
                {
                    loaded.emplace(std::move(spec), nullptr);
                    visit(*cell);
                }
            }
        }
    }

    CellSquare squareAt(const osg::Vec3f& position)
    {
        const auto square
            = [](float value) { return static_cast<int>(std::floor(value / static_cast<float>(ESM::Cell::sSize))); };

        return CellSquare{ .mX = square(position.x()), .mY = square(position.y()) };
    }

    std::string cellAt(const CellSquare& square)
    {
        return std::to_string(square.mX) + ',' + std::to_string(square.mY);
    }

    std::uint32_t dropCellsOutside(World& world, const ESM::Cell& centre, osg::Group& root, Rtx::SceneDesc& scene,
        Rtx::SceneExtractor& extractor, LoadedCells& loaded)
    {
        if (!centre.isExterior())
            return 0;

        std::vector<osg::Vec2i> square;
        gridAround(centre).listCells(square);

        std::set<std::string> keep;
        for (const osg::Vec2i& position : square)
            keep.insert(std::to_string(position.x()) + ',' + std::to_string(position.y()));

        std::uint32_t went = 0;
        for (auto entry = loaded.begin(); entry != loaded.end();)
        {
            if (keep.contains(entry->first))
            {
                ++entry;
                continue;
            }

            if (entry->second.mNode != nullptr)
                root.removeChild(entry->second.mNode);

            // **The ground goes with the references standing on it.** They arrive by two routes —
            // the cell's own group, and the one node `Terrain::TerrainGrid` accumulates into — so
            // taking the group off the root drops only half of what the cell brought.
            if (const ESM::Cell* left = world.findCell(entry->first))
                world.unloadTerrain(left->getGridX(), left->getGridY());

            entry = loaded.erase(entry);
            ++went;
        }

        return went;
    }

    CellReport readRegion(World& world, const ESM::Cell& centre, osg::Group& root, Rtx::SceneDesc& scene,
        Rtx::SceneExtractor& extractor, LoadedCells& loaded, bool liveProps)
    {
        CellReport report;

        // **The cells this call actually brought, and only those.** The grid walk is what decides
        // which they are, and it decides once: it adds every square to `loaded` as it goes, so
        // asking it a second time would find nothing new and asking it against a fresh map would
        // find all nine — reading, instancing and re-parenting the six that were already standing.
        // That is what leaked, and it leaked two thirds of a grid per crossing.
        //
        // Pointers into the loaded content, which outlives every call.
        std::vector<const ESM::Cell*> arrived;

        // **Before a chunk is built, and it is the same grid the walk below fills.** The terrain
        // reads it as the square it must not page, so a grid that said anything else would leave
        // the cells between the two answers with their ground and none of their statics.
        if (centre.isExterior())
            world.setActiveCellGrid(gridAround(centre));

        // Terrain first, because `World::buildTerrain` accumulates chunks under one node and hands
        // that same node back each time: the objects go under groups of their own, and the ground
        // has to be in the graph before anything walks it.
        osg::ref_ptr<osg::Group> terrain;
        forEachNewCell(world, centre, loaded, [&](const ESM::Cell& cell) {
            ++report.mCells;
            arrived.push_back(&cell);
            if (osg::ref_ptr<osg::Group> chunks = world.buildTerrain(cell))
                terrain = std::move(chunks);
        });

        // **Hung under the root once, and it accumulates from there.** `World::buildTerrain` keeps
        // every chunk under one node and hands that same node back each call, so this is the same
        // node every time — adding it again would put the whole worldspace under the root twice.
        //
        // Terrain is therefore not per cell the way the objects below are, and what a departing
        // cell takes with it comes off through `World::unloadTerrain` instead.
        if (terrain != nullptr && !root.containsNode(terrain))
            root.addChild(terrain);

        for (const ESM::Cell* cell : arrived)
        {
            LoadedCell& entry = loaded[keyOf(*cell)];
            entry.mNode = readObjects(world, *cell, root, report, liveProps);
        }

        return report;
    }

    namespace
    {
        osg::ref_ptr<osg::Group> readObjects(
            World& world, const ESM::Cell& cell, osg::Group& root, CellReport& report, bool liveProps)
        {
            // **One group per cell, so a cell can leave the way it arrived.** The game parents every
            // reference under the cell it belongs to; taking that group off the root is what
            // unloading will be, and a flat root would leave nothing to take.
            osg::ref_ptr<osg::Group> group = new osg::Group;
            group->setName(cell.mName.empty() ? "cell" : cell.mName);

            const World::SkippedObjects skipped = world.forEachObject(cell, [&](const World::Object& object) {
                if (object.mPerson != nullptr)
                {
                    report.mPeople.push_back(
                        CellPerson{ .mRecord = object.mPerson, .mTransform = object.mTransform, .mParent = group });
                    return;
                }

                osg::ref_ptr<osg::MatrixTransform> where = new osg::MatrixTransform(osg::Matrixd(object.mTransform));

                // **A light with no mesh has nothing to load and nothing to instance**, and goes
                // straight to `addLight` below. `World::forEachObject` says why it arrives at all.
                if (!object.mModel.empty())
                {
                    osg::ref_ptr<osg::Node> node;
                    try
                    {
                        // **An instance per reference, which is what the game makes.** A shared
                        // template is one node walked under a hundred paths, so a hundred crates
                        // were one placement between them until an anchor was invented to tell them
                        // apart. Give each its own node and the node path identifies it again,
                        // exactly as it does in the game — and the anchor stops being needed.
                        node = world.getSceneManager().getInstance(object.mModel);
                    }
                    catch (const std::exception& e)
                    {
                        Log(Debug::Warning) << "Cannot load " << object.mModel << ": " << e.what();
                        ++report.mUnreadable;
                        return;
                    }

                    // **The same test `SceneManager::getInstance` makes**, and for the same reason:
                    // a particle emitter is an update callback, so a graph with none of those has
                    // nothing that changes between frames. Everything else in a cell is still, and
                    // a template shared by every reference of the model is the cheaper thing to
                    // walk.
                    //
                    // Reported *instead of* mirrored, because the instance somebody makes of it
                    // shares these very drawables and would place the same candle a second time.
                    const bool prop = liveProps
                        && (node->getUpdateCallback() != nullptr || node->getNumChildrenRequiringUpdateTraversal() > 0);

                    // The model goes in first where it is going in at all, so that `addLight` below
                    // can find an `AttachLight` node inside it.
                    if (prop)
                        report.mProps.push_back(
                            CellProp{ .mModel = object.mModel, .mTransform = object.mTransform, .mParent = group });
                    else
                        where->addChild(node);
                }

                // **A `LIGH` reference's light goes into the graph, exactly as the game puts it
                // there.** Read out of the record into a list instead, it was something no walk
                // could ever meet — so the sweep that empties the light table on the frame a cell
                // departs had nothing to refill it from, and every lamp went out on the first
                // crossing. `SceneUtil::addLight` also honours the `AttachLight` node a model may
                // carry, which is how a lantern's flame sits at the wick rather than at the origin.
                //
                // **And it happens whether or not the model went to the props.** A lantern whose
                // flame has to be instanced somewhere it can run still stands where it stood and
                // still lights the street; attaching after the prop test lost every one of them.
                //
                // **And not at all for a record that does not cast where it stands.** The game builds
                // no light source for one (`Rtx::castsWherePlaced` says which), so there is none in
                // its graph for a walk to find — and a `LightSource` carries no flag the mirror could
                // read the refusal off, which is why the graph route decides it here.
                if (object.mLight != nullptr && Rtx::castsWherePlaced(*object.mLight))
                    // **The mirror does not filter on it**, so it decides nothing here. It is what
                    // the game marks a light node with, so the two graphs look the same to anything
                    // that ever does.
                    SceneUtil::addLight(
                        where, SceneUtil::LightCommon(*object.mLight), SceneUtil::Mask_Lighting, cell.isExterior());

                // A prop with no light leaves an empty transform, which is nothing to place.
                if (where->getNumChildren() > 0)
                    group->addChild(where);
            });

            if (group->getNumChildren() > 0)
                root.addChild(group);

            report.mSkipped.mUnknownType += skipped.mUnknownType;
            report.mSkipped.mNoModel += skipped.mNoModel;
            return group;
        }
    }

    RegionLoad loadRegion(World& world, const ESM::Cell& centre, osg::Group& root, Rtx::SceneDesc& scene,
        Rtx::SceneExtractor& extractor, LoadedCells& loaded, std::string_view weather, int day, float hour,
        bool liveProps)
    {
        CellReport report = readRegion(world, centre, root, scene, extractor, loaded, liveProps);

        // **The sheet is the world's and not the region's**, so whoever owns it says where it is.
        // Left at never here, and `StagedWorld` writes what its own plane answers.
        const float level = -std::numeric_limits<float>::infinity();

        // **A room is lit out of its own record, sun included.** `Rtx::makeRoomLight` is what the
        // game does with an `AMBI`, so a `shot` of a room stands under the light a played frame
        // does; the weather and the hour are an exterior's business and decide nothing here.
        if (!centre.isExterior())
        {
            const Rtx::Daylight room = Rtx::makeRoomLight(centre);
            return RegionLoad{ .mLighting
                = CellLighting{ .mAmbient = room.mAmbient, .mWaterLevel = level, .mDaylight = room, .mFog = room.mFog },
                .mReport = std::move(report) };
        }

        const Rtx::Daylight daylight = Rtx::makeDaylight(weather, hour);

        // **After the daylight, and that is what makes the `value` safe.** A name that is none of
        // the ten throws out of the fallback map on the line above, so anything that reaches here
        // is a weather the table knows.
        const std::uint32_t identity = Rtx::weatherIndex(weather).value();

        return RegionLoad{ .mLighting = CellLighting{ .mAmbient = daylight.mAmbient,
                               .mWaterLevel = level,
                               .mDaylight = daylight,
                               .mOutdoors = true,
                               .mDay = day,
                               .mHour = hour,
                               .mWeather = identity,
                               .mFog = daylight.mFog },
            .mReport = std::move(report) };
    }
}
