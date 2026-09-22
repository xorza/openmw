#include "glground.hpp"

#include <utility>

#include <osg/Vec2i>

#include <components/terrain/world.hpp>

#include "../mwworld/cellstore.hpp"
#include "../mwworld/ptr.hpp"
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

    namespace
    {
        /// The exterior cell a reference stands in, as the paging indexes its chunks by.
        osg::Vec2i cellOf(const MWWorld::ConstPtr& ptr)
        {
            const MWWorld::Cell& cell = *ptr.getCell()->getCell();
            return osg::Vec2i(cell.getGridX(), cell.getGridY());
        }
    }

    // Upstream's, from RenderingManager::pagingEnableObject. The position and the cell are what
    // this paging finds a reference's chunk by, and no other renderer reads them.
    bool GlGround::enableReference(const int type, const MWWorld::ConstPtr& ptr, const bool enabled)
    {
        if (!mObjectPaging)
            return false;
        if (mObjectPaging->enableObject(
                type, ptr.getCellRef().getRefNum(), ptr.getCellRef().getPosition().asVec3(), cellOf(ptr), enabled))
        {
            mTerrain->rebuildViews();
            return true;
        }
        return false;
    }

    // Upstream's, from RenderingManager::pagingBlacklistObject.
    bool GlGround::blacklistReference(const int type, const MWWorld::ConstPtr& ptr)
    {
        if (!mObjectPaging)
            return false;
        if (mObjectPaging->blacklistObject(
                type, ptr.getCellRef().getRefNum(), ptr.getCellRef().getPosition().asVec3(), cellOf(ptr)))
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
