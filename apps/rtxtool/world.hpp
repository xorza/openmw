#pragma once

#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <osg/Vec3f>
#include <osg/ref_ptr>

#include <components/misc/cellgrid.hpp>

#include <components/rtx/distantlights.hpp>
#include <components/rtx/terrainresidency.hpp>

#include "terrainstorage.hpp"

namespace Resource
{
    class ImageManager;
    class ResourceSystem;
    class SceneManager;
}

namespace Terrain
{
    class ObjectPaging;
    class World;
}

namespace osg
{
    class Group;
}

namespace ESM
{
    struct Cell;
}

namespace RtxTool
{
    class Content;

    /// How much world the harness builds, in units.
    ///
    /// **One reading for the tool, the way `MWRender::RtxRenderer` keeps one for the game.**
    /// `components/rtx` holds no settings registry, so each host answers this out of
    /// `[RTX] distant land cells` and `[Camera] viewing distance` for itself.
    float landReach();

    /// Whether the game merges the statics inside its active grid into paged chunks.
    ///
    /// **What this world cannot build the way the game does**, and `buildTerrain` says why it
    /// cannot. So where this answers yes, the same exterior stands a different instance count and a
    /// different set of acceleration structures in the two hosts, and a row taken here cannot be
    /// read against one taken there.
    ///
    /// **The tool's own `--distant-terrain` and `--distant-statics` do not enter it**, because this
    /// world merges the active grid under none of them. Neither does the game's `distant terrain`:
    /// `RtxRenderer::wantsPagedTerrain` answers yes whatever it says, since rays go everywhere.
    bool gameMergesActiveGridStatics();

    /// A Morrowind world stood up with no window and no game running.
    ///
    /// Everything OpenMW builds between the content files and a frame — the resource managers that
    /// turn a model path into a scene graph, and the terrain that would otherwise arrive by cull —
    /// and nothing above it. No GL context is created or needed: contexts are for drawing, not for
    /// loading.
    ///
    /// **What the content is stays in `Content`, which this borrows and does not own.** Standing a
    /// world costs a tenth of a millisecond against the eighty reading the content does, so a caller
    /// that wants two worlds over one installation — the harness comparing a paged world with a
    /// gridded one, or a test that must not inherit what the last test built — takes a second of
    /// these and no second read.
    class World
    {
    public:
        /// @param content outlives this, and is unchanged by it.
        explicit World(const Content& content);
        ~World();

        World(const World&) = delete;
        World& operator=(const World&) = delete;

        /// What the content files say, which this stands on and never alters.
        const Content& getContent() const { return mContent; }

        /// Loads an exterior cell's terrain and returns the graph it went into. Null for an
        /// interior, and for an exterior with no land record.
        ///
        /// The graph is the same one every time and it accumulates: asking for a second cell adds
        /// its chunks beside the first's. Nothing here loads more than one, and a caller that did
        /// would want them together anyway.
        ///
        /// The game gets terrain without asking: by cull time `Terrain::QuadTreeWorld` has already
        /// put chunks in the scene graph, and the mirror picks them up like any other geometry.
        /// Headless there is no such thing, so the harness stands one up — and the renderer still
        /// does not have to know terrain exists, which is the whole argument for mirroring a graph
        /// rather than reading the content files twice.
        ///
        /// The returned node lives until `clearTerrain`, and from there for as long as whoever hung
        /// it under a graph holds it.
        osg::ref_ptr<osg::Group> buildTerrain(const ESM::Cell& cell);

        /// Says which square of cells is loaded, which the terrain needs and the caller owns.
        ///
        /// **Told rather than accumulated from the cells that arrive.** `Terrain::ObjectPaging`
        /// reads this square as the one it must not page, because the caller stands those cells
        /// itself — so a square that only ever widened left every cell the run had passed through
        /// in neither picture: its references unloaded, and its distant statics still refused. The
        /// caller derives this from the centre exactly as it derives the loaded set, which is what
        /// keeps the two from disagreeing. See `Misc::CellGrid`.
        void setActiveCellGrid(const Misc::CellGrid& grid);

        /// Takes one exterior cell's chunks back out of that graph.
        ///
        /// **The other half of `buildTerrain`, and the harness went without it for a while.** A
        /// working set that only ever gains ground is a benchmark measuring a world no player holds:
        /// nine cells of ashland became twenty-nine over a walk across the island, and the frames
        /// that reported were not the game's. Harmless before anything streamed and wrong the moment
        /// something did.
        void unloadTerrain(int x, int y);

        /// Puts the terrain back to never built, so the next region staged here stands on its own
        /// ground.
        ///
        /// **This `World` outlives a `StagedWorld` and its terrain outlived it too.** A region
        /// staged second therefore stood on what the first left: with the quad tree off, the cells
        /// nothing had unloaded, walked straight into the second region's scene; with it on, the
        /// chunks `Terrain::ObjectPaging` had merged, which leave out whatever square was active
        /// when they were built and are cached against the chunk rather than the square. Measured,
        /// staging Balmora before the island crossing left the crossing with a hundred and
        /// forty-four instances of another place's ground under the grid, and with the quad tree
        /// three instances, three textures and 5.8% of the pixels.
        void clearTerrain();

        /// The node `buildTerrain` accumulates into, or null before the first exterior and after
        /// `clearTerrain`.
        ///
        /// For a caller that wants to know which chunks are new: the count before it loads and the
        /// count after bound exactly the ones it caused.
        osg::Group* getTerrainRoot() const { return mTerrainParent.get(); }

        /// Whether the terrain is paged the way the game pages it with `distant terrain` on.
        ///
        /// **What makes this worth an option at all**: `Terrain::QuadTreeWorld` keeps its chunks out
        /// of the scene graph, so it is the one terrain a mirror cannot find by walking — and the
        /// harness building only `Terrain::TerrainGrid` meant nothing here could see that.
        ///
        /// Read when the terrain is built, so a world that has built it ignores this.
        void pageTerrain(bool paged) { mPagedTerrain = paged; }

        /// Whether the distant ground carries what stands on it — the buildings, trees and rocks the
        /// game merges into a chunk through `Terrain::ObjectPaging`.
        ///
        /// **The A/B that says what they cost**, which is the whole reason this is separable from
        /// the ground it stands on. Ignored where nothing pages, and where the game's own
        /// `object paging` is off.
        ///
        /// Read when the terrain is built, so a world that has built it ignores this.
        void pageStatics(bool paged) { mPagedStatics = paged; }

        /// Everything the graph does not parent: the terrain's chunks, and the lights of the cells
        /// the paging leaves dark. Empty for a world that parents its ground.
        std::span<Rtx::Residency* const> getResidencies() const { return mResidencies; }

        /// How far out a paged world produces chunks at all, in world units.
        ///
        /// **`viewing distance` is the rasterizer's fog-and-visibility knob**, and its default of
        /// 7168 is smaller than the 8192 a cell is — so a paged world left alone produces nothing
        /// outside the active grid, whatever the LOD would have done with it. What a ray tracer
        /// needs is how much world exists, which is a property of the structure rays are cast
        /// against and not of the camera.
        ///
        /// Never called leaves `viewing distance` in charge, which is what everything not looking
        /// for distance gets.
        void setTerrainViewDistance(float units);

        /// Where a paged world chooses its detail from. Ignored where nothing pages.
        void setTerrainViewPoint(const osg::Vec3f& where);

        Resource::SceneManager& getSceneManager();

        Resource::ImageManager& getImageManager();

        /// For what wants a manager this does not hand out one by one — the keyframes an actor is
        /// posed by, and the virtual file system they are looked up in.
        Resource::ResourceSystem& getResourceSystem() { return *mResourceSystem; }
        const Resource::ResourceSystem& getResourceSystem() const { return *mResourceSystem; }

    private:
        const Content& mContent;

        std::unique_ptr<Resource::ResourceSystem> mResourceSystem;

        // Destruction runs backwards through what follows, and that is what orders it.
        //
        // The terrain is built on the first exterior asked for. Its destructor unloads every cell,
        // detaches its root and deregisters it from the resource system, so it has to go before
        // both the storage it read and the system it registered with.
        std::unique_ptr<TerrainStorage> mTerrainStorage;

        osg::ref_ptr<osg::Group> mTerrainParent;
        osg::ref_ptr<osg::Group> mCompileRoot;

        // The quad tree holds a bare pointer to the paging as one of its chunk managers, so the
        // paging has to outlive the tree — which is what putting it above `mTerrain` says.
        std::unique_ptr<Terrain::ObjectPaging> mObjectPaging;
        std::unique_ptr<Terrain::World> mTerrain;
        bool mPagedTerrain = false;
        bool mPagedStatics = true;

        /// Unset until something asks for distance, and `viewing distance` stands in for it.
        std::optional<float> mTerrainViewDistance;

        /// Non-null only for a paged world, which is the only one that hides its chunks.
        std::unique_ptr<Rtx::TerrainResidency> mResident;

        /// **Beside the chunks and on the same terms.** A `LIGH` is not a paged type, so a distant
        /// lantern has no node for any walk to find; this reads them out of the content files
        /// instead. Held whether or not the world pages, and handed over only when it does — there
        /// is nothing out there for a light to fall on otherwise.
        Rtx::DistantLights mDistantLights;

        /// The two above as the extractor takes them, refilled when the terrain is built or cleared.
        std::vector<Rtx::Residency*> mResidencies;

        /// The square of cells the caller says is loaded. See `setActiveCellGrid`.
        Misc::CellGrid mActiveGrid;
    };
}
