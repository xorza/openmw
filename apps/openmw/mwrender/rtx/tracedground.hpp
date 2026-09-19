#pragma once

#include <vector>

#include <osg/Vec2i>
#include <osg/Vec3f>
#include <osg/Vec4i>

#include <components/esm/refid.hpp>
#include <components/esm3/refnum.hpp>

#include "../ground.hpp"
#include "tracedterrain.hpp"

namespace osg
{
    class Group;
}

namespace Terrain
{
    class Storage;
}

namespace MWRender
{
    class RtxRenderer;

    /// The ground and the distance as the ray tracer has them: a `TracedTerrain` that draws no
    /// chunks, and the mirror's cell ring, which stands the distant statics itself. What the game
    /// says of a reference reaches the ring through this and the renderer that owns it, the way
    /// it reaches the paging through `GlGround`.
    class TracedGround final : public Ground
    {
    public:
        TracedGround(osg::Group& sceneRoot, Terrain::Storage& storage, unsigned int nodeMask, ESM::RefId worldspace,
            RtxRenderer& renderer);

        Terrain::World& getTerrain() override { return mTerrain; }

        /// Told to the ring, and false: the ring stands and drops on its own walk, so the game has
        /// nothing to rebuild here.
        bool enableReference(
            int type, ESM::RefNum refnum, const osg::Vec3f& position, const osg::Vec2i& cell, bool enabled) override;
        bool blacklistReference(
            int type, ESM::RefNum refnum, const osg::Vec3f& position, const osg::Vec2i& cell) override;

        /// The ring keeps no cache of the game's toggles to unlock.
        bool unlockCache() override { return false; }

        /// Nothing: the ring stands nothing inside the active grid.
        void collectPagedRefnums(const osg::Vec4i& activeGrid, std::vector<ESM::RefNum>& out) override {}

        void clear() override;

    private:
        TracedTerrain mTerrain;

        /// Borrowed: the renderer that made this outlives it, and the ring is its.
        RtxRenderer& mRenderer;
    };
}
