#pragma once

#include <memory>
#include <vector>

#include <osg/Vec4i>

#include <components/esm3/refnum.hpp>

#include "ground.hpp"

namespace Terrain
{
    class World;
}

namespace MWRender
{
    class Groundcover;
    class ObjectPaging;

    /// The ground as the rasterizer builds it: upstream's chunked world with the paging and the
    /// groundcover that ride on its chunks. The five answers below are upstream's, from
    /// `RenderingManager`'s paging members, with a null paging as "object paging is off".
    class GlGround final : public Ground
    {
    public:
        GlGround(std::unique_ptr<Terrain::World> terrain, std::unique_ptr<ObjectPaging> paging,
            std::unique_ptr<Groundcover> groundcover);

        /// Out of line, where the three are complete.
        ~GlGround() override;

        Terrain::World& getTerrain() override { return *mTerrain; }

        bool enableReference(int type, const MWWorld::ConstPtr& ptr, bool enabled) override;
        bool blacklistReference(int type, const MWWorld::ConstPtr& ptr) override;
        bool unlockCache() override;
        void collectPagedRefnums(const osg::Vec4i& activeGrid, std::vector<ESM::RefNum>& out) override;
        void clear() override;

    private:
        std::unique_ptr<Terrain::World> mTerrain;
        std::unique_ptr<ObjectPaging> mObjectPaging;
        std::unique_ptr<Groundcover> mGroundcover;
    };
}
