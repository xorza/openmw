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
#include <components/sceneutil/lightcommon.hpp>

#include "content.hpp"
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

        /// What this reference lights the room with, or nothing for the great majority that light
        /// nothing.
        ///
        /// **Carried to the instance rather than stood here, because the mesh went with it.**
        /// `Rtx::standLight` hangs a light on the model's `AttachLight` node where the model has
        /// one, which is the wick of a lantern and the flame of a candle; a prop's model is not in
        /// the graph for that search to find, so a light stood beside it sat at the reference's own
        /// origin instead — up to 48 units below the wick, on ten of one room's 26 lamps.
        ///
        /// The description and not the record, because that is what every rule about a light reads.
        std::optional<SceneUtil::LightCommon> mLight;

        /// Carried because the cell is gone by the time `Rtx::standLight` runs, and it decides the
        /// attenuation the light is given.
        bool mExterior = false;
    };

    /// What a cell brought: the group its references hang under.
    ///
    /// **A group per cell is what makes a cell able to leave.** Taking that node off the root is the
    /// whole of unloading: the next walk does not reach what was under it, and the sweep that
    /// follows takes its placements, its meshes and its materials with it. Its lights go the same
    /// way, being `LightSource` nodes exactly as the game makes them.
    struct LoadedCell
    {
        osg::ref_ptr<osg::Group> mNode;
    };

    /// Which cells are in the graph, by the name `--cell` spells each one with.
    using LoadedCells = std::map<std::string, LoadedCell>;

    /// Takes every cell outside the active grid around `centre` off the graph. Returns how many.
    ///
    /// **Both halves of what a cell brought.** Its references hang under a group of their own and
    /// its ground under the one node `Terrain::TerrainGrid` accumulates into, so a departure is a
    /// child removed from the root *and* an `unloadCell` — and dropping only the first leaves a
    /// working set that gains ground for as long as the camera flies.
    std::uint32_t dropCellsOutside(World& world, const ESM::Cell& centre, osg::Group& root, LoadedCells& loaded);

    /// What reading a cell produced besides the graph itself.
    struct CellReport
    {
        Content::SkippedObjects mSkipped;

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
    };

    /// Builds a region's graph under `root`, and reports what it holds that a walk will not find.
    ///
    /// **The graph and not the scene.** What puts a region into a `Rtx::SceneDesc` is the walk a
    /// caller makes afterwards, so this reads content and parents nodes and nothing else.
    ///
    /// **The lamps go into the graph and not into the report**, exactly as the game places them:
    /// `NifOsg` never reads `NiLight`, so a `LIGH` reference's light is a `SceneUtil::LightSource`
    /// hung beside its mesh, where every walk that mirrors the graph meets it again.
    ///
    /// @param centre the cell asked for, and the middle of the square of exterior cells read
    ///        around it. An interior has no neighbours and is read alone. Cells the content files
    ///        do not define are open sea and are skipped rather than missing.
    /// @param loaded which cells are already in the graph. Cells named here are left alone and
    ///        every cell this places is added to it, so a caller that keeps one across calls walks
    ///        into a region rather than reloading it.
    /// @param liveProps whether a reference whose model carries an update callback is *left out* of
    ///        the graph and reported in `mProps` instead. **Because it has to be one or the other.**
    ///        A prop that is going to be instanced and stepped brings its own copy of the same
    ///        geometry — the clone shares the drawables — so mirroring the template as well would
    ///        stand two candles in one place. A caller with nowhere to keep an instance passes false
    ///        and gets the still template, which is a candle with an authored spark on it.
    CellReport readRegion(World& world, const ESM::Cell& centre, osg::Group& root, LoadedCells& loaded, bool liveProps);

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
    /// is a cell record there, which is what `Content::findCell` says by answering nothing.
    std::string cellAt(const CellSquare& square);

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

    /// Everything a region puts into `scene`, and how its centre is lit.
    ///
    /// Geometry and lights through `extractor` and `scene`, and the sky, water and air as the
    /// return. **In the library rather than beside `main` because it has three callers now** — the
    /// screenshot, the window, and the test that needs a frame of real content to measure.
    RegionLoad loadRegion(World& world, const ESM::Cell& centre, osg::Group& root, Rtx::SceneDesc& scene,
        Rtx::SceneExtractor& extractor, LoadedCells& loaded, std::string_view weather, int day, float hour,
        bool liveProps);
}
