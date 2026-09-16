#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <osg/Matrixf>

#include "mesh.hpp"
#include "runs.hpp"
#include "slots.hpp"

namespace Rtx
{
    /// Where every mesh stands, where it stood, and which rows a backend has to write again. A
    /// slot is never moved and never closed up, because a hit reads its slot index back. The two
    /// change lists are the whole of what a backend rewrites: a world is tens of thousands of
    /// placements and a frame changes hundreds. An arrival takes the lowest free slot and never the
    /// last one freed, because the slot settles a tie between coincident sheets and the order a
    /// sweep drops slots in is a hash of node addresses.
    class PlacementTable
    {
    public:
        /// Puts `instance` in a free slot, or in a new one, and returns it. `SceneDesc::addInstance`
        /// is the way in, for the asserts it makes across the tables.
        Index add(const MeshInstance& instance);

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

        /// Says every row wearing `material` has to be written again for a reason this table did
        /// not make — the material changed what traversal is told about the surfaces standing on
        /// it. The placements that wear it and no other: a list per material is threaded through
        /// the slots, so a fade crossing opaque costs its own placements rather than a walk of the
        /// world's.
        void rewriteWearing(Index material);

        /// Ends a placement a backend took: what moved becomes where things were. Costs what moved
        /// and not what stands. What was moved becomes `getSettled`, and `getMoved` starts empty —
        /// so only after a backend has read both, because a slot that leaves the lists unread is a
        /// row no copy of the tables is ever told about.
        void advance();

        /// Every slot, standing or empty, in slot order. `MeshInstance::isPlaced` tells them apart.
        std::span<const MeshInstance> getAll() const { return mInstances.getRows(); }

        /// How many slots hold a placement, which is what reaches an acceleration structure.
        std::uint32_t getPlacedCount() const { return mPlacedCount; }

        /// Where each slot stood before the last `advance`, indexed alongside the slots.
        std::span<const osg::Matrixf> getPrevious() const { return mPrevious; }

        /// The slots whose row changed since the last `advance`: placed, moved, faded, dropped, or
        /// wearing a material that changed what traversal is told. A slot can appear more than once.
        std::span<const Index> getMoved() const { return mMoved; }

        /// The slots the last `advance` caught up, whose motion is now still — the other half of
        /// what a backend rewrites, or last frame's motion would stay in the row for ever.
        std::span<const Index> getSettled() const { return mSettled; }

    private:
        /// Puts `slot` at the head of `material`'s list, and takes it out again. Nothing for a
        /// placement wearing no material.
        void link(Index slot, Index material);
        void unlink(Index slot, Index material);

        SlotRows<MeshInstance> mInstances;
        std::vector<osg::Matrixf> mPrevious;

        /// The list of slots wearing each material, doubly linked through the slots and headed by
        /// material: `mFirstWearing[material]` is the newest placement wearing it, and each slot
        /// names the ones before and after it. Parallel to the slots and to the materials, grown
        /// with each, and `sNoIndex` at every end.
        std::vector<Index> mNextWearing;
        std::vector<Index> mPrevWearing;
        std::vector<Index> mFirstWearing;

        /// Plain lists that hold duplicates, where every other change list in this scene is a
        /// `SlotSet`: a slot named twice is a memcpy of a hundred bytes, bounded by the three facts
        /// about a placement that can change in a frame.
        std::vector<Index> mMoved;
        std::vector<Index> mSettled;

        std::uint32_t mPlacedCount = 0;
    };
}
