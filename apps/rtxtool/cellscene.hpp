#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <osg/Group>
#include <osg/Matrixf>
#include <osg/Vec3f>

#include <components/rtx/scenedesc.hpp>
#include <components/rtx/sceneextractor.hpp>

#include "lighting.hpp"
#include "world.hpp"

namespace ESM
{
    struct Cell;
    struct NPC;
}

namespace RtxTool
{
    /// One person a cell places, and where they stand.
    ///
    /// **Reported rather than built, for the reason the lights are.** A body is a graph the extractor
    /// keys its meshes on, so whoever owns it has to own it for as long as the scene names it — and
    /// reading a region twice must not stand two of everyone in it.
    struct CellPerson
    {
        /// Points into the loaded content, which outlives the run.
        const ESM::NPC* mRecord = nullptr;
        osg::Matrixf mTransform;

        /// The group this cell's references hang under, which is what an actor has to hang under
        /// too.
        ///
        /// **A person leaves with their cell or stands in an empty street.** Everything else a cell
        /// brings is under this node, so taking it off the root is the whole of unloading; actors
        /// went under the run's own root instead and outlived the town around them.
        osg::ref_ptr<osg::Group> mParent;
    };

    /// One reference a cell places whose model is not still, and where it stands.
    ///
    /// **Reported rather than instanced, for the reason a resident is.** An instance is a graph the
    /// extractor keys its meshes on, so whoever owns it has to own it for as long as the scene names
    /// it — and reading a region twice must not light the same candle twice.
    struct CellProp
    {
        VFS::Path::Normalized mModel;
        osg::Matrixf mTransform;

        /// The group this cell's references hang under. See `CellPerson::mParent`.
        osg::ref_ptr<osg::Group> mParent;
    };

    /// Which cells are in the graph, and the group each one's references hang under.
    ///
    /// **A group per cell is what makes a cell able to leave.** Taking that node off the root is the
    /// whole of unloading: the next walk does not reach what was under it, and the sweep that
    /// follows takes its placements, its meshes and its materials with it.
    /// What a cell brought.
    ///
    /// **Its water quad, and that is now the whole of it.** A cell's references hang under a group
    /// and a walk finds them every time — its lights included, since those are `LightSource` nodes
    /// exactly as the game makes them. The water is the exception: an analytic quad goes straight
    /// into the scene, so nothing can re-find it and the cell is the only thing that knows when it
    /// should go.
    struct LoadedCell
    {
        osg::ref_ptr<osg::Group> mNode;
    };

    using LoadedCells = std::map<std::string, LoadedCell>;

    /// Takes every cell outside the active grid around `centre` off the graph. Returns how many.
    ///
    /// **Both halves of what a cell brought.** Its references hang under a group of their own and
    /// its ground under the one node `Terrain::TerrainGrid` accumulates into, so a departure is a
    /// child removed from the root *and* an `unloadCell` — and dropping only the first leaves a
    /// working set that gains ground for as long as the camera flies.
    std::uint32_t dropCellsOutside(World& world, const ESM::Cell& centre, osg::Group& root, Rtx::SceneDesc& scene,
        Rtx::SceneExtractor& extractor, LoadedCells& loaded);

    /// What reading a cell produced besides the scene itself.
    struct CellReport
    {
        World::SkippedObjects mSkipped;

        /// References whose model is named but will not load. Logged individually as they fail.
        std::uint32_t mUnreadable = 0;

        /// Everyone the region places. Empty is a wilderness cell, not a failure.
        std::vector<CellPerson> mPeople;

        /// The references whose template carries an update callback, which in Morrowind's content
        /// means a particle emitter and nothing else. Their still geometry is already in the scene;
        /// what is not is the flame, and that needs an instance of its own to run in.
        std::vector<CellProp> mProps;

        /// How many cells the region actually found. Fewer than asked for at a coastline.
        std::uint32_t mCells = 0;

        /// The cell's ambient, linear. Black for an exterior, whose sky is M5's.
        osg::Vec3f mAmbient;
    };

    /// Mirrors one cell's geometry through `extractor`, and reports what else it holds.
    ///
    /// The content arrives by two routes and only one of them is the scene graph: lights are not
    /// in it at all, because `NifOsg` never reads `NiLight`. They come off the `LIGH` records the
    /// same references point at, and leave here in the report.
    /// The same, over a square of exterior cells centred on `centre`.
    ///
    ///        Ignored for an interior, which has no neighbours. Cells the content files do not
    ///        define are open sea and are skipped rather than missing.
    /// @param loaded which cells are already in the scene. Cells named here are left alone and
    ///        every cell this places is added to it, so a caller that keeps one across calls walks
    ///        into a region rather than reloading it.
    /// @param liveProps whether a reference whose model carries an update callback is *left out* of
    ///        the scene and reported in `mProps` instead. **Because it has to be one or the other.**
    ///        A prop that is going to be instanced and stepped brings its own copy of the same
    ///        geometry — the clone shares the drawables — so mirroring the template as well would
    ///        stand two candles in one place. A caller with nowhere to keep an instance passes false
    ///        and gets the still template, which is a candle with an authored spark on it.
    CellReport readRegion(World& world, const ESM::Cell& centre, osg::Group& root, Rtx::SceneDesc& scene,
        Rtx::SceneExtractor& extractor, LoadedCells& loaded, bool liveProps);

    /// Which exterior square a point stands in.
    ///
    /// **Two integers rather than the string, because a streaming frame asks every frame.** Naming
    /// the square is how a crossing is noticed, and spelling it out to find that nothing has changed
    /// is two allocations on the frame path for an answer that is almost always no.
    struct CellSquare
    {
        int mX = 0;
        int mY = 0;

        friend bool operator==(const CellSquare& a, const CellSquare& b) = default;
    };

    CellSquare squareAt(const osg::Vec3f& position);

    /// The exterior cell a point stands in, as `--cell` spells it.
    ///
    /// A point outside every cell the content files define still has a square: what it does not have
    /// is a cell record there, which is what `World::findCell` says by answering nothing.
    std::string cellAt(const CellSquare& square);

    /// Everything a region puts into `scene`, and how its centre is lit.
    ///
    /// Geometry and lights through `extractor` and `scene`, and the sky, water and air as the
    /// return. **In the library rather than beside `main` because it has three callers now** — the
    /// screenshot, the window, and the test that needs a frame of real content to measure.
    /// What loading a region left for its caller to place.
    ///
    /// The lights and the water are already in the scene; these two are not, because neither is
    /// something a second read of the same region may do twice — a light has no identity to
    /// recognise, and a person is a graph whose owner has to outlive the scene that names it.
    struct RegionLoad
    {
        CellLighting mLighting;

        /// What reading the region found, whole. **Carried rather than picked over**, because a
        /// caller that only wanted the people used to be the only caller: `scene` reports the
        /// skipped counts and the lights off the same load the renderer is handed, and it can only
        /// do that if the load hands them over.
        CellReport mReport;
    };

    RegionLoad loadRegion(World& world, const ESM::Cell& centre, osg::Group& root, Rtx::SceneDesc& scene,
        Rtx::SceneExtractor& extractor, LoadedCells& loaded, std::string_view weather, int day, float hour,
        bool liveProps);
}
