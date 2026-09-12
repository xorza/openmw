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
    /// The lights of the cells the paging leaves dark. `REC_LIGH` is not a paged type and must not
    /// become one, because both renderers read `Terrain::pagedType`; what this fork cannot keep is
    /// the light, because rays go everywhere. A `Residency` and not a graph: what it builds is
    /// parented to nothing the rasterizer walks, and the mirror turns it into a `Rtx::Light`
    /// exactly as it does a lamp in the active cell. Outside the active grid only, or a lantern is
    /// counted twice.
    class DistantLights final : public Residency
    {
    public:
        DistantLights();
        ~DistantLights() override;

        DistantLights(const DistantLights&) = delete;
        DistantLights& operator=(const DistantLights&) = delete;

        /// Where the world is now. A storage of null is a world with none. `mOutdoors` false is an
        /// interior, whose coordinates belong to another space; what has been read stays read,
        /// because a door is walked through both ways.
        void follow(const WorldAround& around) override;

        /// Drops what has been read, so the next `collect` builds it again — for a host that
        /// restarts `SceneUtil::resetLightIds`, because `Rtx::lightPhase` turns a light's id into
        /// where its flame stands in its cycle.
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

        /// Every cell read so far, sorted by grid position — content, read once for the life of the
        /// world, and the null entries kept because the absence is the answer. Sorted and searched
        /// rather than keyed, because `collect` looks up eighty-one cells every frame and inserts
        /// only the first time each is seen. Emptied by `follow` and by `restart`.
        std::vector<ReadCell> mCells;

        /// Refilled per cell built. A cell arrives while the game runs, so its read allocates no more
        /// freely than a frame does.
        std::vector<Terrain::PagedCellRef> mRefScratch;
    };
}
