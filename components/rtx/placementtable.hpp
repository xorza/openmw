#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <osg/Matrixf>

#include "index.hpp"
#include "meshinstance.hpp"

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

        void clear();

        /// Every slot, standing or empty, in slot order. `MeshInstance::isPlaced` tells them apart.
        std::span<const MeshInstance> getAll() const { return mInstances; }

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
        std::vector<MeshInstance> mInstances;
        std::vector<osg::Matrixf> mPrevious;
        std::vector<Index> mMoved;
        std::vector<Index> mSettled;
        std::vector<Index> mFree;
        std::uint32_t mPlacedCount = 0;
    };
}
