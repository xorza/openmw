#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <span>
#include <vector>

#include "index.hpp"

namespace Rtx
{
    /// The slots of a table that nothing stands in.
    ///
    /// **The lowest is what a take answers with, never the last one freed.** Every row is the same
    /// size, so any hole would hold the next arrival — but which one it takes decides what a run
    /// draws. `Rtx::Identity` hashes by address, so a sweep retires in whatever order the allocator
    /// left its map in, and a list taken from the back then hands the same live set different slots
    /// in two processes. The lowest makes the answer a function of what is standing rather than of
    /// the order the dead left in. Measured on `one-cell-walk`: taken from the back, the `materials`
    /// and `textures` columns differed from frame 2 on 89 frames of 90.
    ///
    /// **`apps/rtxtool/repeatable.sh` is what asks it now.** The reading above is one run of a
    /// question the gate puts to every scene column of every pair, so a take that went back to the
    /// last one freed fails there rather than going quiet until somebody measures again.
    ///
    /// **Its own type, because three tables keep one.** `SlotRows` holds the rows beside it;
    /// `GuiTextures` and `VulkanRenderer`'s view scenes hold rows this cannot — a `unique_ptr` is
    /// move-only and `SlotRows::take` copies. What they share is the policy, so the policy is what
    /// is shared.
    class SlotPool
    {
    public:
        /// A free slot, or `sNoIndex` where there is none and the caller has to append.
        Index take()
        {
            if (mFree.empty())
                return sNoIndex;

            std::pop_heap(mFree.begin(), mFree.end(), std::greater<>());
            const Index index = mFree.back();
            mFree.pop_back();

            return index;
        }

        /// Puts `slot` back, so the next `take` may answer with it.
        ///
        /// **The only way onto the list.** A slot pushed onto it by anything else leaves it no heap,
        /// and the pop above is then undefined and answers with whatever the top happens to be.
        void free(Index slot)
        {
            mFree.push_back(slot);
            std::push_heap(mFree.begin(), mFree.end(), std::greater<>());
        }

        /// How many slots stand empty, which a table subtracts from its length to count what lives.
        std::size_t size() const { return mFree.size(); }

        /// Every empty slot, in the heap's own order rather than in any order a reader may want.
        ///
        /// **For a caller that walks all of them**, which is what a mark does: a sweep runs on the
        /// frame a cell left, and asking this per row instead would be a search per row.
        std::span<const Index> getSlots() const { return mFree; }

    private:
        /// A min-heap of the slots nothing stands in.
        std::vector<Index> mFree;
    };
}
