#include "cellscene.hpp"

#include <osg/MatrixTransform>

#include <components/misc/constants.hpp>

#include <algorithm>
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
#include <components/rtx/fogbuilder.hpp>
#include <components/rtx/lightbuilder.hpp>
#include <components/rtx/texturebuilder.hpp>
#include <components/sceneutil/lightcommon.hpp>
#include <components/sceneutil/lightutil.hpp>
#include <components/weather/downpour.hpp>

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
        /// The square of cells the game keeps active, and the order it fills it in.
        ///
        /// **Copied from `MWWorld::Scene`, deliberately, rather than shared.** The originals —
        /// `iterateOverCellsAround` and `sortCellsToLoad` — sit in an anonymous namespace in
        /// `apps/openmw/mwworld/scene.cpp`, so nothing outside that file can link to them, and
        /// lifting them into `components/` costs three upstream files to share twenty lines of
        /// arithmetic. Twenty lines is the cheaper copy.
        ///
        /// **What it has to keep is the order.** Nearest first, ties broken by distance to the
        /// origin. The scanline fill this replaced put the cells in an order the game never uses,
        /// which for a benchmark that times a camera crossing a boundary is the whole measurement.
        /// If `Scene` ever changes how it sorts, this has to follow it, and nothing here will say
        /// so — that is what the copy costs.
        std::vector<std::pair<int, int>> squareAround(int centreX, int centreY)
        {
            const int range = Constants::CellGridRadius;
            const auto side = static_cast<std::size_t>(2 * range + 1);

            std::vector<std::pair<int, int>> square;
            square.reserve(side * side);

            for (int x = centreX - range; x <= centreX + range; ++x)
                for (int y = centreY - range; y <= centreY + range; ++y)
                    square.emplace_back(x, y);

            const auto priority = [&](const std::pair<int, int>& cell) {
                return std::make_pair(std::abs(cell.first - centreX) + std::abs(cell.second - centreY),
                    std::abs(cell.first) + std::abs(cell.second));
            };

            std::sort(square.begin(), square.end(),
                [&](const std::pair<int, int>& a, const std::pair<int, int>& b) { return priority(a) < priority(b); });

            return square;
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

            for (const auto& [x, y] : squareAround(centre.getGridX(), centre.getGridY()))
            {
                std::string spec = std::to_string(x) + ',' + std::to_string(y);
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

        std::set<std::string> keep;
        for (const auto& [x, y] : squareAround(centre.getGridX(), centre.getGridY()))
            keep.insert(std::to_string(x) + ',' + std::to_string(y));

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

        // Interiors were authored against a renderer with no bounce, so the cell's own ambient
        // is most of what lights one. An exterior's `AMBI` is the weather system's business and
        // is not read here.
        if (!centre.isExterior() && centre.mHasAmbi)
            report.mAmbient = Rtx::decodeColour(centre.mAmbi.mAmbient);

        for (const ESM::Cell* cell : arrived)
        {
            LoadedCell& entry = loaded[keyOf(*cell)];
            entry.mNode = readObjects(world, *cell, root, report, liveProps);
        }

        return report;
    }

    namespace
    {
        /// What the game marks a light node with (`MWRender::Mask_Lighting`).
        ///
        /// **The mirror does not filter on it**, so it decides nothing here; it is the game's value
        /// so that the two graphs look the same to anything that ever does.
        constexpr unsigned int sLightMask = 1u << 19;

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

                osg::ref_ptr<osg::Node> node;
                try
                {
                    // **An instance per reference, which is what the game makes.** A shared template
                    // is one node walked under a hundred paths, so a hundred crates were one
                    // placement between them until an anchor was invented to tell them apart. Give
                    // each its own node and the node path identifies it again, exactly as it does
                    // in the game — and the anchor stops being needed.
                    node = world.getSceneManager().getInstance(object.mModel);
                }
                catch (const std::exception& e)
                {
                    Log(Debug::Warning) << "Cannot load " << object.mModel << ": " << e.what();
                    ++report.mUnreadable;
                    return;
                }

                // **The same test `SceneManager::getInstance` makes**, and for the same reason: a
                // particle emitter is an update callback, so a graph with none of those has nothing
                // that changes between frames. Everything else in a cell is still, and a template
                // shared by every reference of the model is the cheaper thing to walk.
                //
                // Reported *instead of* mirrored, because the instance somebody makes of it shares
                // these very drawables and would place the same candle a second time.
                osg::ref_ptr<osg::MatrixTransform> where = new osg::MatrixTransform(osg::Matrixd(object.mTransform));

                const bool prop = liveProps
                    && (node->getUpdateCallback() != nullptr || node->getNumChildrenRequiringUpdateTraversal() > 0);

                // The model goes in first where it is going in at all, so that `addLight` below can
                // find an `AttachLight` node inside it.
                if (prop)
                    report.mProps.push_back(
                        CellProp{ .mModel = object.mModel, .mTransform = object.mTransform, .mParent = group });
                else
                    where->addChild(node);

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
                if (object.mLight != nullptr)
                {
                    const osg::ref_ptr<SceneUtil::LightSource> source = SceneUtil::addLight(
                        where, SceneUtil::LightCommon(*object.mLight), sLightMask, cell.isExterior());

                    // **A prop's transform carries the light and nothing else, and `addLight` calls
                    // that empty.** The flag exists so the game can skip a light on something with
                    // nothing to draw; here the model is not missing, it is being instanced
                    // somewhere it can run its flame. Left alone this dropped every candle, every
                    // lamp and every torch in the game — twenty-three of them in the census office
                    // alone, which reported no lights at all.
                    if (prop)
                        source->setEmpty(false);
                }

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

        // An interior's sky is never seen and its sun never shines, so the daylight stays dark.
        // Its air is its own, out of the same `AMBI` its ambient came from.
        if (!centre.isExterior())
            return RegionLoad{ .mLighting = CellLighting{ .mAmbient = report.mAmbient,
                                   .mWaterLevel = level,
                                   .mDaylight = {},
                                   .mFog = Rtx::interiorFog(centre) },
                .mReport = std::move(report) };

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
                               .mWindSpeed = Weather::windSpeed(weather),
                               .mFog = daylight.mFog },
            .mReport = std::move(report) };
    }
}
