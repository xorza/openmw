#ifndef GAME_RENDER_GROUND_H
#define GAME_RENDER_GROUND_H

#include <vector>

#include <osg/Vec2i>
#include <osg/Vec3f>
#include <osg/Vec4i>

#include <components/esm/refid.hpp>
#include <components/esm3/refnum.hpp>

namespace osg
{
    class Group;
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
    /// What the game has for a worldspace's ground and hands a renderer to build it from.
    struct GroundSpec
    {
        osg::Group& mSceneRoot;
        osg::Group& mWorldRoot;
        Terrain::Storage& mStorage;
        const MWWorld::GroundcoverStore& mGroundcoverStore;
        ESM::RefId mWorldspace;
    };

    /// One worldspace's ground and the distance over it, as a renderer builds them: the terrain
    /// world the game drives — the active grid, the view distance, the height cull — and whatever
    /// stands the statics the content files put beyond the loaded cells. The rasterizer pages them
    /// onto its chunks, the ray tracer stands them in its own ring, and the game tells both the
    /// same things through this: a script's toggle, a moved object, a new game.
    class Ground
    {
    public:
        virtual ~Ground() = default;

        Ground(const Ground&) = delete;
        Ground& operator=(const Ground&) = delete;

        virtual Terrain::World& getTerrain() = 0;

        /// A script enabled or disabled the exterior reference `refnum` of record type `type`,
        /// which stands at `position` in `cell`. @return whether what the distance draws changed
        /// for it, which is what upstream's paging says where it dropped or restored a chunk.
        virtual bool enableReference(
            int type, ESM::RefNum refnum, const osg::Vec3f& position, const osg::Vec2i& cell, bool enabled)
            = 0;

        /// The game moved, deleted or animates the reference, so the distance must never stand it
        /// again, whatever a script says of it later. @return as `enableReference`.
        virtual bool blacklistReference(
            int type, ESM::RefNum refnum, const osg::Vec3f& position, const osg::Vec2i& cell)
            = 0;

        /// The cell grid has moved: what the game told the distance while the grid changed may now
        /// be applied. @return whether what the distance draws changed.
        virtual bool unlockCache() = 0;

        /// Adds to `out` every reference inside `activeGrid` the distance draws itself, so the game
        /// does not stand it a second time.
        virtual void collectPagedRefnums(const osg::Vec4i& activeGrid, std::vector<ESM::RefNum>& out) = 0;

        /// A new game or a load: nothing a script said about a reference holds any more.
        virtual void clear() = 0;

    protected:
        Ground() = default;
    };
}

#endif
