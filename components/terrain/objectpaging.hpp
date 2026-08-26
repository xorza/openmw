#ifndef OPENMW_COMPONENTS_TERRAIN_OBJECTPAGING_H
#define OPENMW_COMPONENTS_TERRAIN_OBJECTPAGING_H

#include <components/esm3/refnum.hpp>
#include <components/resource/resourcemanager.hpp>
#include <components/terrain/objectstorage.hpp>
#include <components/terrain/quadtreeworld.hpp>

#include <mutex>

namespace Resource
{
    class SceneManager;
}

namespace Terrain
{

    typedef std::tuple<osg::Vec2f, float, bool> ChunkId; // Center, Size, ActiveGrid

    class ObjectPaging : public Resource::GenericResourceManager<ChunkId>, public QuadTreeWorld::ChunkManager
    {
    public:
        /// @param storage what the content files say stands where. Borrowed, and must outlive this.
        /// @param nodeMask what the chunks this builds are drawn under — the caller's own static
        ///        mask, because a component has no opinion about the renderer's bins.
        /// @param pageActiveGrid whether the cells the caller has loaded are merged into chunks too,
        ///        rather than only the distance. **The caller's to state and not a setting's to
        ///        decide**, because whoever says yes owes the other half of the bargain: the
        ///        references a chunk swallowed must not also be stood up one by one, and only the
        ///        caller knows how it stands them. The game reads its setting and asks
        ///        `getPagedRefnums` to find out what to skip; a caller with no such machinery says
        ///        no here and keeps the paging to the distance, which is the same picture by a
        ///        different route.
        ObjectPaging(Resource::SceneManager* sceneManager, const ObjectStorage& storage, ESM::RefId worldspace,
            unsigned int nodeMask, bool pageActiveGrid);
        ~ObjectPaging() = default;

        osg::ref_ptr<osg::Node> getChunk(float size, const osg::Vec2f& center, unsigned char lod, unsigned int lodFlags,
            bool activeGrid, const osg::Vec3f& viewPoint, bool compile) override;

        osg::ref_ptr<osg::Node> createChunk(float size, const osg::Vec2f& center, bool activeGrid,
            const osg::Vec3f& viewPoint, bool compile, unsigned char lod);

        unsigned int getNodeMask() override;

        /// @return true if view needs rebuild
        bool enableObject(int type, ESM::RefNum refnum, const osg::Vec3f& pos, const osg::Vec2i& cell, bool enabled);

        /// @return true if view needs rebuild
        bool blacklistObject(int type, ESM::RefNum refnum, const osg::Vec3f& pos, const osg::Vec2i& cell);

        void clear();

        /// Must be called after clear() before rendering starts.
        /// @return true if view needs rebuild
        bool unlockCache();

        void reportStats(unsigned int frameNumber, osg::Stats* stats) const override;

        void getPagedRefnums(const osg::Vec4i& activeGrid, std::vector<ESM::RefNum>& out);

    private:
        Resource::SceneManager* mSceneManager;
        const ObjectStorage* mStorage;
        unsigned int mNodeMask;
        bool mActiveGrid;
        bool mDebugBatches;
        float mMergeFactor;
        float mMinSize;
        float mMinSizeMergeFactor;
        float mMinSizeCostMultiplier;

        std::mutex mRefTrackerMutex;
        struct RefTracker
        {
            std::set<ESM::RefNum> mDisabled;
            std::set<ESM::RefNum> mBlacklist;
            bool operator==(const RefTracker& other) const
            {
                return mDisabled == other.mDisabled && mBlacklist == other.mBlacklist;
            }
        };
        RefTracker mRefTracker;
        RefTracker mRefTrackerNew;
        bool mRefTrackerLocked;

        const RefTracker& getRefTracker() const { return mRefTracker; }
        RefTracker& getWritableRefTracker() { return mRefTrackerLocked ? mRefTrackerNew : mRefTracker; }

        std::mutex mSizeCacheMutex;
        typedef std::map<ESM::RefNum, float> SizeCache;
        SizeCache mSizeCache;

        std::mutex mLODNameCacheMutex;
        typedef std::pair<std::string, unsigned char> LODNameCacheKey; // Key: mesh name, lod level
        using LODNameCache = std::map<LODNameCacheKey, VFS::Path::Normalized>; // Cache: key, mesh name to use
        LODNameCache mLODNameCache;
    };

    class RefnumMarker : public osg::Object
    {
    public:
        RefnumMarker()
            : mNumVertices(0)
        {
        }
        RefnumMarker(const RefnumMarker& copy, osg::CopyOp co)
            : mRefnum(copy.mRefnum)
            , mNumVertices(copy.mNumVertices)
        {
        }
        META_Object(Terrain, RefnumMarker)

        ESM::RefNum mRefnum;
        unsigned int mNumVertices;
    };
}

#endif
