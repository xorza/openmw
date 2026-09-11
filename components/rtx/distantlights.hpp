#pragma once

#include <vector>

#include <osg/Vec2i>
#include <osg/ref_ptr>

#include <components/terrain/objectstorage.hpp>

#include "residency.hpp"

namespace osg
{
    class Group;
}

namespace Terrain
{
    class ObjectStorage;
}

namespace Rtx
{
    /// The lights of the cells the paging leaves dark.
    ///
    /// **`REC_LIGH` is not a paged type, and it must not become one.** `Terrain::pagedType` decides
    /// what a distant hillside is made of and both renderers read it, so a lantern four cells away
    /// has no model in either — that is upstream's picture and changing it would change the
    /// rasterizer's. What this fork cannot keep is the *light*: rays go everywhere, so a town that
    /// exists at dusk but casts nothing is the world stating something the content files do not.
    ///
    /// **A `Residency` and not a graph.** What it builds is parented to nothing the rasterizer
    /// walks, handed to the mirror on the frame that asks, and gone from every other question
    /// anybody puts to the scene. The mirror then turns a `SceneUtil::LightSource` into a
    /// `Rtx::Light` exactly as it does for a lamp in the cell the player stands in — flicker, pulse
    /// and negative light included — so nothing here computes what a light is.
    ///
    /// **Outside the active grid and nowhere else.** Inside it the game has stood the real object,
    /// its light is on the graph, and the mirror has already found it; a second copy would be the
    /// same lantern counted twice.
    class DistantLights final : public Residency
    {
    public:
        DistantLights();
        ~DistantLights() override;

        DistantLights(const DistantLights&) = delete;
        DistantLights& operator=(const DistantLights&) = delete;

        /// Where the world is now: what to read, which worldspace of it, and where the eye stands.
        ///
        /// **Every field of it is read here.** A storage of null is a world with none, which
        /// `collect` answers by doing nothing.
        ///
        /// **`mOutdoors` false is an interior, where the eye's coordinates belong to another
        /// space.** The cells the reach would name around it are the exterior cells nearest the
        /// origin of a room, which stand nothing and mean nothing. What has been read stays read: a
        /// door is walked through both ways, and the reading is what this class exists to do once.
        void follow(const WorldAround& around) override;

        /// Drops what has been read, so the next `collect` builds it again.
        ///
        /// **For a host that restarts `SceneUtil::resetLightIds`, and for no other reason.** A light
        /// this built carries an id out of that counter, and `Rtx::lightPhase` turns the id into
        /// where the flame stands in its cycle — so a cell kept across a restart flickers at a phase
        /// from a sequence that no longer exists, and can hold an id the new sequence has since
        /// handed to somebody else. Whoever restarts the counter calls this in the same breath.
        void restart();

        void collect(SceneAdopter& into, ExtractionStats& stats) override;

    private:
        /// One cell that has been read, and what it stood — null where it stood nothing.
        struct ReadCell
        {
            osg::Vec2i mCell;
            osg::ref_ptr<osg::Group> mLights;
        };

        /// Reads one cell's `LIGH` references and stands a light at each. Null where it holds none.
        osg::ref_ptr<osg::Group> build(const osg::Vec2i& cell);

        /// Where the eye is and how much world there is around it, as the last `follow` said.
        WorldAround mAround;

        /// Every cell read so far, sorted by grid position.
        ///
        /// **Content, so a cell is read once for the life of the world.** What a `LIGH` reference
        /// says does not change with the hour or the weather — the flicker is a function of the
        /// simulation time and the mirror applies it every frame — so a cell that has been read
        /// costs a pointer to hand over and nothing else. The null entries are kept for the same
        /// reason: the absence is the answer, and reading the blocks again to find it out is the
        /// cost this avoids.
        ///
        /// **Sorted and searched rather than keyed, because a frame looks up far more often than it
        /// inserts.** `collect` asks about every cell of the reach on every frame — eighty-one of
        /// them at the default — and inserts only the first time each is seen. A node-based map
        /// spends an allocation on each cell and makes every one of those lookups a walk of
        /// pointers; a binary search over one contiguous run costs neither, and the inner loop of
        /// `collect` walks the same order the run is sorted in. The insert's shift is paid on the
        /// frame that also reads the cell's blocks off the disk, which is the cost that frame is
        /// actually about.
        ///
        /// **Emptied by `follow` and by `restart`, and by nothing else**: the first is a world that
        /// changed, the second a scene that began again under the same world.
        std::vector<ReadCell> mCells;

        /// Refilled per cell built. A cell arrives while the game runs, so its read allocates no more
        /// freely than a frame does.
        std::vector<Terrain::PagedCellRef> mRefScratch;
    };
}
