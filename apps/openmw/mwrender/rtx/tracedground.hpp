#pragma once

#include <components/esm/refid.hpp>

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

        /// Told to the ring by its reference number, and false: the ring stands and drops on its
        /// own walk, so the game has nothing to rebuild here. The rest of what the reference says
        /// is the paging's, and the seam's default answers the two questions a ring has no cache
        /// and no grid for.
        bool enableReference(int type, const MWWorld::ConstPtr& ptr, bool enabled) override;
        bool blacklistReference(int type, const MWWorld::ConstPtr& ptr) override;

        void clear() override;

    private:
        TracedTerrain mTerrain;

        /// Borrowed: the renderer that made this outlives it, and the ring is its.
        RtxRenderer& mRenderer;
    };
}
