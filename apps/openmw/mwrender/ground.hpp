#ifndef GAME_RENDER_GROUND_H
#define GAME_RENDER_GROUND_H

#include <vector>

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
    class ConstPtr;
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

        /// A script enabled or disabled the exterior reference `ptr`, of record type `type`.
        /// @return whether what the distance draws changed for it, which is what upstream's paging
        /// says where it dropped or restored a chunk, and what makes the caller reload the terrain.
        ///
        /// **The reference and not the four facts one paging reads off it.** Which of them a
        /// renderer needs is the renderer's: upstream's paging finds the chunk by the position and
        /// the cell, and a ring that stands references itself reads the number alone. The type is
        /// a parameter because it comes out of the world's store, which nothing below this can
        /// reach.
        virtual bool enableReference(int type, const MWWorld::ConstPtr& ptr, bool enabled) = 0;

        /// The game moved, deleted or animates the reference, so the distance must never stand it
        /// again, whatever a script says of it later. @return as `enableReference`.
        virtual bool blacklistReference(int type, const MWWorld::ConstPtr& ptr) = 0;

        /// The cell grid has moved: what the game told the distance while the grid changed may now
        /// be applied. @return whether what the distance draws changed. A renderer that keeps no
        /// such cache has nothing to unlock and answers no.
        virtual bool unlockCache() { return false; }

        /// Adds to `out` every reference inside `activeGrid` the distance draws itself, so the game
        /// does not stand it a second time. A renderer that stands nothing inside the grid adds
        /// nothing.
        virtual void collectPagedRefnums(const osg::Vec4i& activeGrid, std::vector<ESM::RefNum>& out) {}

        /// A new game or a load: nothing a script said about a reference holds any more.
        virtual void clear() = 0;

    protected:
        Ground() = default;
    };
}

#endif
