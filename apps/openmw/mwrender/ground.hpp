#ifndef GAME_RENDER_GROUND_H
#define GAME_RENDER_GROUND_H

#include <memory>

#include <components/esm/refid.hpp>

namespace osg
{
    class Group;
}

namespace Resource
{
    class ResourceSystem;
}

namespace Terrain
{
    class Storage;
    class World;
}

namespace MWWorld
{
    class GroundcoverStore;
}

namespace MWRender
{
    class Groundcover;
    class ObjectPaging;

    /// What the game has for a worldspace's ground and hands a renderer to build it from.
    struct GroundSpec
    {
        osg::Group& mSceneRoot;
        osg::Group& mWorldRoot;
        Resource::ResourceSystem& mResources;
        Terrain::Storage& mStorage;
        const MWWorld::GroundcoverStore& mGroundcoverStore;
        ESM::RefId mWorldspace;
    };

    /// What a renderer built for it: the terrain world the game drives — the active grid, the
    /// view distance, the height cull — and, where the ground is paged, the statics and the
    /// groundcover that ride on its chunks, which the game clears and reports on. Its special
    /// members are defined where the three are complete.
    struct Ground
    {
        Ground();
        Ground(Ground&& other) noexcept;
        Ground& operator=(Ground&& other) noexcept;
        ~Ground();

        std::unique_ptr<Terrain::World> mTerrain;
        std::unique_ptr<ObjectPaging> mObjectPaging;
        std::unique_ptr<Groundcover> mGroundcover;
    };
}

#endif
