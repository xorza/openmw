#include "glground.hpp"

#include <utility>

#include <components/terrain/world.hpp>

#include "groundcover.hpp"
#include "objectpaging.hpp"

namespace MWRender
{
    GlGround::GlGround(std::unique_ptr<Terrain::World> terrain, std::unique_ptr<ObjectPaging> paging,
        std::unique_ptr<Groundcover> groundcover)
        : mTerrain(std::move(terrain))
        , mObjectPaging(std::move(paging))
        , mGroundcover(std::move(groundcover))
    {
    }

    GlGround::~GlGround() = default;

    // Upstream's, from RenderingManager::pagingEnableObject.
    bool GlGround::enableReference(
        int type, ESM::RefNum refnum, const osg::Vec3f& position, const osg::Vec2i& cell, bool enabled)
    {
        if (!mObjectPaging)
            return false;
        if (mObjectPaging->enableObject(type, refnum, position, cell, enabled))
        {
            mTerrain->rebuildViews();
            return true;
        }
        return false;
    }

    // Upstream's, from RenderingManager::pagingBlacklistObject.
    bool GlGround::blacklistReference(int type, ESM::RefNum refnum, const osg::Vec3f& position, const osg::Vec2i& cell)
    {
        if (!mObjectPaging)
            return false;
        if (mObjectPaging->blacklistObject(type, refnum, position, cell))
        {
            mTerrain->rebuildViews();
            return true;
        }
        return false;
    }

    // Upstream's, from RenderingManager::pagingUnlockCache.
    bool GlGround::unlockCache()
    {
        if (mObjectPaging && mObjectPaging->unlockCache())
        {
            mTerrain->rebuildViews();
            return true;
        }
        return false;
    }

    // Upstream's, from RenderingManager::getPagedRefnums.
    void GlGround::collectPagedRefnums(const osg::Vec4i& activeGrid, std::vector<ESM::RefNum>& out)
    {
        if (mObjectPaging)
            mObjectPaging->getPagedRefnums(activeGrid, out);
    }

    // Upstream's, from RenderingManager::clear.
    void GlGround::clear()
    {
        if (mObjectPaging)
            mObjectPaging->clear();
    }
}
