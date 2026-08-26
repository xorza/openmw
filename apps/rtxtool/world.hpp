#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string_view>

#include <osg/Matrixf>
#include <osg/Vec3f>
#include <osg/Vec4i>

#include <components/esm/refid.hpp>
#include <components/esm3/readerscache.hpp>
#include <components/esmloader/esmdata.hpp>
#include <components/files/collections.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/toutf8/toutf8.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>

#include <components/rtx/terrainresidency.hpp>

#include "objectstorage.hpp"
#include "terrainstorage.hpp"

namespace boost::program_options
{
    class variables_map;
}

namespace Files
{
    struct ConfigurationManager;
}

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
    class Node;
}

namespace ESM
{
    struct Position;
    struct Light;
    struct Cell;
    struct NPC;
    struct Region;
}

namespace RtxTool
{
    /// A Morrowind installation, opened with no window and no game running.
    ///
    /// Everything OpenMW builds below its simulation on the way to a frame — the virtual file
    /// system, the content files, and the resource managers that turn a model path into a scene
    /// graph — and nothing above it. No GL context is created or needed: contexts are for drawing,
    /// not for loading.
    class World
    {
    public:
        World(Files::ConfigurationManager& config, const boost::program_options::variables_map& variables,
            const std::filesystem::path& resourcePath);
        ~World();

        World(const World&) = delete;
        World& operator=(const World&) = delete;

        /// Finds a cell the way Morrowind addresses one: a pair of integers is an exterior, anything
        /// else is an interior's name. Null when there is no such cell.
        const ESM::Cell* findCell(std::string_view spec) const;

        /// The region a cell names, or null where it names none or names one nothing defines.
        ///
        /// **What decides which weathers a place ever sees.** Interiors mostly name nothing, and the
        /// caller reads that as "no opinion" rather than "no weather".
        const ESM::Region* findRegion(const ESM::RefId& id) const;

        /// A game setting's number, or `missing` where the content files carry no such setting.
        ///
        /// **For the handful of constants that are settings rather than fallbacks.** Most of what
        /// the weather is made of comes out of `Fallback::Map`, which reads the ini and needs no
        /// store; `fStromWindSpeed` is the exception and the game reads it from here too.
        float findGameSetting(std::string_view id, float missing) const;

        /// One object a cell places.
        struct Object
        {
            VFS::Path::Normalized mModel;
            osg::Matrixf mTransform;

            /// The `LIGH` record this reference stands for, or null. A candle is both things at
            /// once: a mesh to place and a light to cast, arriving by the same reference.
            ///
            /// Points into the loaded content, which outlives every call.
            const ESM::Light* mLight = nullptr;

            /// The `NPC_` record this reference stands for, or null.
            ///
            /// **A person arrives with no model at all.** Everyone else names a file; an NPC record
            /// names a race and a sex, and the body has to be assembled out of the `BODY` records
            /// those call for. So the reference hands over the record and `mModel` stays empty.
            const ESM::NPC* mPerson = nullptr;
        };

        /// What `forEachObject` met but could not place.
        struct SkippedObjects
        {
            /// References to a record type whose model this tool does not read — lights, creatures,
            /// items on the floor. Their geometry is real; loading it needs more of the content
            /// files than the static world does.
            std::uint32_t mUnknownType = 0;

            /// References whose record has no model at all. Markers, mostly.
            std::uint32_t mNoModel = 0;
        };

        /// Calls `handle` for every object the cell places that has a model to draw.
        SkippedObjects forEachObject(const ESM::Cell& cell, const std::function<void(const Object&)>& handle);

        /// Where the game would stand a character who walked into `destination`, if anything leads
        /// there.
        ///
        /// **The arrival a door names, and not the door itself.** A teleporting reference carries the
        /// position its far side puts you at, so what says where an interior is entered is the door
        /// *outside* it — the one in the cell you came from, whose destination is this one. A camera
        /// placed from the interior's own door stands at the way out and looks back in, which is a
        /// different place and, in a winding cave, a wall.
        ///
        /// **Found by walking the world's references, and only when asked.** Nothing indexes this at
        /// load: it would cost every run a pass over every reference in the game to answer a question
        /// only a view with no camera ever asks. The walk stops at the first door that names
        /// `destination`, and what it found is kept for the rest of the run.
        std::optional<ESM::Position> findArrival(const ESM::Cell& destination);

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
        /// The returned node lives as long as this `World` does.
        osg::ref_ptr<osg::Group> buildTerrain(const ESM::Cell& cell);

        /// Takes one exterior cell's chunks back out of that graph.
        ///
        /// **The other half of `buildTerrain`, and the harness went without it for a while.** A
        /// working set that only ever gains ground is a benchmark measuring a world no player holds:
        /// nine cells of ashland became twenty-nine over a walk across the island, and the frames
        /// that reported were not the game's. Harmless before anything streamed and wrong the moment
        /// something did.
        void unloadTerrain(int x, int y);

        /// The node `buildTerrain` accumulates into, or null before the first exterior.
        ///
        /// For a caller that wants to know which chunks are new: the count before it loads and the
        /// count after bound exactly the ones it caused.
        osg::Group* getTerrainRoot() const { return mTerrainParent.get(); }

        /// Whether the terrain is paged the way the game pages it with `distant terrain` on.
        ///
        /// **What makes this worth an option at all**: `Terrain::QuadTreeWorld` keeps its chunks out
        /// of the scene graph, so it is the one terrain a mirror cannot find by walking — and the
        /// harness building only `Terrain::TerrainGrid` meant nothing here could see that.
        void pageTerrain(bool paged) { mPagedTerrain = paged; }

        /// Whether the distant ground carries what stands on it — the buildings, trees and rocks the
        /// game merges into a chunk through `Terrain::ObjectPaging`.
        ///
        /// **The A/B that says what they cost**, which is the whole reason this is separable from
        /// the ground it stands on. Ignored where nothing pages, and where the game's own
        /// `object paging` is off.
        void pageStatics(bool paged) { mPagedStatics = paged; }

        /// The terrain's chunks where the graph does not parent them, or null where it does.
        Rtx::Residency* getTerrainResidency() { return mResident.get(); }

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

        /// Every record of one type the content files carry, sorted by id.
        ///
        /// For what a cell reference cannot answer: a person is assembled out of the `BODY` records
        /// their race calls for, and nothing places those — they are looked up, not referenced.
        template <class T>
        const std::vector<T>& getRecords() const
        {
            return mEsmData.get<T>();
        }

        /// One record by id, or null. Linear over the type, which is what the callers want it for:
        /// a handful of lookups while something is being built, never a frame.
        template <class T>
        const T* findRecord(const ESM::RefId& id) const
        {
            for (const T& record : getRecords<T>())
                if (record.mId == id)
                    return &record;

            return nullptr;
        }

    private:
        // Declaration order is destruction order reversed, and the managers hold references to the
        // encoder and the VFS, so those come first and go last.
        ToUTF8::Utf8Encoder mEncoder;
        Files::Collections mFileCollections;
        VFS::Manager mVfs;
        ESM::ReadersCache mReaders;

        // Built in the initialiser list: `EsmData` is move-constructible and not assignable, which is
        // the right shape for something this size and means it cannot be filled in from the body.
        EsmLoader::EsmData mEsmData;
        std::unique_ptr<Resource::ResourceSystem> mResourceSystem;

        // Built on the first exterior asked for. The grid keeps the chunks it made: its destructor
        // unloads every cell and detaches its root, so it has to outlive whoever is reading them.
        //
        // Declaration order here is load-bearing, because destruction runs backwards through it.
        // The grid deregisters itself from the resource system, so it must go first; the storage
        // holds pointers into `mEsmData`, so it must go before that.
        std::unique_ptr<TerrainStorage> mTerrainStorage;

        // What the paging reads the world out of, and the paging itself. The quad tree holds a bare
        // pointer to the latter as one of its chunk managers, so it has to outlive the tree — which
        // is what putting it above `mTerrain` says.
        ObjectStorage mObjectStorage;
        osg::ref_ptr<osg::Group> mTerrainParent;
        osg::ref_ptr<osg::Group> mCompileRoot;
        std::unique_ptr<Terrain::ObjectPaging> mObjectPaging;
        std::unique_ptr<Terrain::World> mTerrain;
        bool mPagedTerrain = false;
        bool mPagedStatics = true;

        /// Unset until something asks for distance, and `viewing distance` stands in for it.
        std::optional<float> mTerrainViewDistance;

        /// Non-null only for a paged world, which is the only one that hides its chunks.
        std::unique_ptr<Rtx::TerrainResidency> mResident;

        /// The square of cells the paged world is told to hold, grown as cells are loaded. A grid
        /// nothing has been put in yet holds no ground at all.
        std::optional<osg::Vec4i> mActiveGrid;

        /// Arrivals found so far, by the cell they lead to. Nothing here holds a reference to
        /// anything above, so it sits outside the ordering the comment at the top of these members
        /// is about. A walk that found nothing is remembered as nothing, so a second ask for the
        /// same cell does not walk the world again.
        std::map<std::string, std::optional<ESM::Position>, Misc::StringUtils::CiComp> mArrivals;
    };
}
