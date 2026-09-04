#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "index.hpp"

namespace Rtx
{
    /// The slots of one table that something is true of, in the order they were named, each once.
    ///
    /// **The list and the byte together, because neither is the answer on its own.** The list is
    /// what a frame walks; the byte is what keeps a slot named twice in a frame from appearing
    /// twice in it. Kept apart, the two fall out of step in the one direction nothing catches — a
    /// slot in the list whose byte says it is not — and the invariant is then maintained wherever
    /// somebody remembers to.
    ///
    /// **What stops the list being searched.** A slot is named once however many callers reach it,
    /// and asking a vector made that N²/2 comparisons for the N movers of a crowded cell — 55,000
    /// of them at Vivec, on every frame, for a count the cell decides rather than one this code
    /// sets.
    class SlotSet
    {
    public:
        /// Grows the per-slot bytes to `count`, which a table does as it takes a slot.
        ///
        /// A resize to the size it already is does not allocate, which is what the frame path pays.
        void grow(std::size_t count) { mFlags.resize(count, 0); }

        /// Puts `slot` in the set, once however many times it is named.
        void add(Index slot)
        {
            assert(slot < mFlags.size() && "a slot the table has not grown to");
            if (mFlags[slot] != 0)
                return;

            mFlags[slot] = 1;
            mSlots.push_back(slot);
        }

        /// Takes `slot` out. Nothing where it was not in the set.
        ///
        /// **The list is left holding it until `compact` runs**, because a sweep takes thousands of
        /// slots back and erasing from the middle of a list of hundreds is that many moves apiece.
        /// `getSlots` will not answer while one is outstanding.
        void remove(Index slot)
        {
            assert(slot < mFlags.size() && "a slot the table has not grown to");
            if (mFlags[slot] == 0)
                return;

            mFlags[slot] = 0;
            mStale = true;
        }

        /// Drops what `remove` took, in one pass over the list rather than one per slot.
        void compact();

        std::span<const Index> getSlots() const
        {
            assert(!mStale && "the list was read between a remove and the compact that settles it");
            return mSlots;
        }

        /// Empties the set.
        ///
        /// **Only the slots in it are put back**, rather than the whole table: a worldspace is
        /// thousands of slots and what a frame names is tens. The bytes keep their room, because a
        /// table emptied is exactly when the next one is about to be filled.
        void clear();

    private:
        std::vector<Index> mSlots;

        /// A byte per slot of the table beside it, set for exactly the slots `mSlots` names —
        /// except between a `remove` and the `compact` that settles it.
        std::vector<std::uint8_t> mFlags;

        bool mStale = false;
    };
}
