#pragma once

#include <span>
#include <vector>

#include <osg/Matrixf>

#include "material.hpp"
#include "mesh.hpp"
#include "runs.hpp"
#include "slots.hpp"

namespace Rtx
{
    /// One slot of the table: the placement, and everything the table knows about the slot beside
    /// it. One row and not a row with arrays beside it, so nothing can fall out of step with the
    /// slot it describes.
    struct PlacementRow
    {
        MeshInstance mInstance;

        /// Where the slot stood before the last `advance`, which is what a motion vector is the
        /// difference of.
        osg::Matrixf mPrevious;

        /// What traversal is told about the material the placement wears, as it stood when the
        /// placement was stood or the material last rewritten. Kept here so a fade, a drop and a
        /// record read one answer without the material table; a material rewrite brings it here
        /// through `rewriteWearing`. Default for a placement wearing nothing.
        Material::Traversed mWorn;

        /// The slots wearing the same material, doubly linked and headed by
        /// `PlacementTable::mFirstWearing`, so a material rewrite reaches its wearers alone.
        Index mNextWearing = sNoIndex;
        Index mPrevWearing = sNoIndex;
    };

    /// Where every mesh stands, where it stood, what each counts as and which rows a backend has
    /// to write again. A slot is never moved and never closed up, because a hit reads its slot
    /// index back. The two change lists are the whole of what a backend rewrites: a world is tens
    /// of thousands of placements and a frame changes hundreds. An arrival takes the lowest free
    /// slot and never the last one freed, because the slot settles a tie between coincident sheets
    /// and the order a sweep drops slots in is a hash of node addresses.
    ///
    /// **The counts are kept here, by the row that changed.** What a placement counts as is
    /// decided by its material's traversal facts, its class and its opacity, all of which this
    /// table is the last to see; a backend that recounted from the rows it was handed kept a
    /// second flag per row to take a departed row's share back, and this table still has the row.
    class PlacementTable
    {
    public:
        /// Puts `instance` in a free slot, or in a new one, and returns it. `SceneDesc::addInstance`
        /// is the way in, for the asserts it makes across the tables and the traversal facts it
        /// reads off the material.
        Index add(const MeshInstance& instance, const Material::Traversed& worn);

        /// Fades the placement in `slot`. Separate from `move`, because an actor fading on the spot
        /// has not moved; a fade that changed the number joins `getMoved` all the same, as a row to
        /// rewrite that carries no motion. The walk's alone: the ring stands and drops, and never
        /// fades.
        void fade(Index slot, float opacity);

        /// Moves the placement in `slot`, and says whether that changed anything. A transform equal
        /// to the one already there writes nothing, which is the ordinary case. The walk's alone,
        /// as `fade` is.
        bool move(Index slot, const osg::Matrixf& transform);

        /// Empties `slot`. Its index is not reused until the next `add` asks for one. The slot
        /// joins `getMoved`: a backend has to write its row inactive, or the structure goes on
        /// tracing what stood there.
        ///
        /// @param by who is dropping it, which must be who stood it — `Stander`.
        void drop(Index slot, Stander by);

        /// Says every row wearing `material` now wears `worn` and has to be written again — the
        /// material changed what traversal is told about the surfaces standing on it. The
        /// placements that wear it and no other: a list per material is threaded through the
        /// slots, so a fade crossing opaque costs its own placements rather than a walk of the
        /// world's.
        void rewriteWearing(Index material, const Material::Traversed& worn);

        /// Ends a placement a backend took: what moved becomes where things were. Costs what moved
        /// and not what stands. What was moved becomes `getSettled`, and `getMoved` starts empty —
        /// so only after a backend has read both, because a slot that leaves the lists unread is a
        /// row no copy of the tables is ever told about.
        void advance();

        /// Every slot, standing or empty, in slot order. `MeshInstance::isPlaced` tells them apart.
        std::span<const PlacementRow> getRows() const { return mRows.getRows(); }

        /// How many slots hold a placement and how many of those each kind of traversal has to
        /// stop for, as the rows stand now.
        const InstanceCounts& getCounts() const { return mCounts; }

        /// The slots whose row changed since the last `advance`: placed, moved, faded, dropped, or
        /// wearing a material that changed what traversal is told. A slot can appear more than once.
        std::span<const Index> getMoved() const { return mMoved; }

        /// The slots the last `advance` caught up, whose motion is now still — the other half of
        /// what a backend rewrites, or last frame's motion would stay in the row for ever.
        std::span<const Index> getSettled() const { return mSettled; }

    private:
        /// What a standing row counts as — one placed, and one of each figure its material and its
        /// class put it in: the one rule, which `InstanceRecord`'s traversal flags are the other
        /// reading of. A translucent row is never asked the cutout's question, so it is not counted
        /// against the cutout's cost however its material is marked.
        static InstanceCounts shareOf(const PlacementRow& row);

        /// Adds the row's share to the counts, and takes it back again. The row is read both
        /// times: every change to what it counts as takes it out first and puts it back after.
        void count(Index slot);
        void discount(Index slot);

        /// Puts `slot` at the head of `material`'s list, and takes it out again. Nothing for a
        /// placement wearing no material.
        void link(Index slot, Index material);
        void unlink(Index slot, Index material);

        SlotRows<PlacementRow> mRows;

        /// The newest placement wearing each material, `sNoIndex` where none does. Parallel to the
        /// materials and grown with them.
        std::vector<Index> mFirstWearing;

        /// Plain lists that hold duplicates, where every other change list in this scene is a
        /// `SlotSet`: a slot named twice is a memcpy of a hundred bytes, bounded by the three facts
        /// about a placement that can change in a frame.
        std::vector<Index> mMoved;
        std::vector<Index> mSettled;

        InstanceCounts mCounts;
    };
}
