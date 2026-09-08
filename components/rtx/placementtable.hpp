#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <osg/Matrixf>

#include "index.hpp"
#include "meshinstance.hpp"
#include "slotrows.hpp"

namespace Rtx
{
    /// Where every mesh stands, where it stood, and which rows a backend has to write again.
    ///
    /// **A slot is never moved and never closed up.** A hit reads its slot index back, and every
    /// acceleration structure a frame keeps is addressed by one — so a dropped placement leaves a
    /// hole for the next arrival to take over, and everything above it goes on meaning what it
    /// meant.
    ///
    /// **The two change lists are the whole of what a backend rewrites.** A world is tens of
    /// thousands of placements and a frame changes hundreds; writing the row table whole was a
    /// millisecond of the game's CPU to change nothing.
    ///
    /// **An arrival takes the lowest free slot, and never the last one freed.** A slot decides two
    /// things a picture depends on: the custom index a hit reads back, and where the placement sits
    /// among the rows a top-level structure is built over — which is what settles a tie between two
    /// surfaces at one distance, and Morrowind's foliage is coincident sheets. The free list was a
    /// stack, so the slot an arrival took followed the order the last sweep dropped its slots in,
    /// and that order is `SceneExtractor`'s map walked in bucket order over keys hashed from node
    /// addresses. Taking the lowest instead makes the slot a function of which slots are free, and
    /// that is a fact about the world rather than about the allocator.
    class PlacementTable
    {
    public:
        /// Puts `instance` in a free slot, or in a new one.
        Index add(const MeshInstance& instance);

        void fade(Index slot, float opacity);

        /// @return whether it moved at all, which is what spares a caller the row.
        bool move(Index slot, const osg::Matrixf& transform);

        void drop(Index slot);

        /// Says `slot`'s row has to be written again for a reason this table did not make — a
        /// material that changed what traversal is told about the surfaces standing on it.
        void rewrite(Index slot) { mMoved.push_back(slot); }

        /// Catches every moved slot up, so that a frame after a move carries no motion.
        void advance();

        /// Every slot, standing or empty, in slot order. `MeshInstance::isPlaced` tells them apart.
        std::span<const MeshInstance> getAll() const { return mInstances.getRows(); }

        /// How many slots hold a placement, which is what reaches an acceleration structure.
        std::uint32_t getPlacedCount() const { return mPlacedCount; }

        /// Where each slot stood before the last `advance`, indexed alongside the slots.
        std::span<const osg::Matrixf> getPrevious() const { return mPrevious; }

        /// The slots whose row changed since the last `advance`: placed, moved, faded, dropped, or
        /// wearing a material that changed what traversal is told.
        ///
        /// A slot can appear more than once where two facts about it changed in one frame, which
        /// costs one row written twice.
        std::span<const Index> getMoved() const { return mMoved; }

        /// The slots the last `advance` caught up, whose motion is now still.
        ///
        /// **The other half of what a backend rewrites.** A row carries the motion between where a
        /// placement stood and where it stands, and that motion goes back to nothing on the frame
        /// after the move — which is a frame on which the slot did not move. Without this list a
        /// backend writing only `getMoved` would leave last frame's motion in the row for ever.
        std::span<const Index> getSettled() const { return mSettled; }

    private:
        SlotRows<MeshInstance> mInstances;
        std::vector<osg::Matrixf> mPrevious;

        /// **Plain lists that hold duplicates, where every other change list in this scene is a
        /// `SlotSet`.** A slot named twice is one row written twice, which is a memcpy of a hundred
        /// bytes; how often that can happen is bounded by how many facts about one placement can
        /// change in a frame, which is three. What `SlotSet` was measured on is the other case — a
        /// list of hundreds asked whether it already holds a slot, once per mover of a crowded
        /// cell, which is the N²/2 comparisons these never make.
        std::vector<Index> mMoved;
        std::vector<Index> mSettled;

        std::uint32_t mPlacedCount = 0;
    };
}
