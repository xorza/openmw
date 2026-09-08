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
    ///
    /// **One type, and everything that keeps such a list holds one.** What arrived and what went is
    /// two of these; a copy's debt is one of these beside an everything flag; a block table's owed
    /// runs are one per copy. Each is the same argument, and a place that makes it again is a place
    /// that can make it wrong.
    class SlotSet
    {
    public:
        /// Makes room for at least `count` slots, which a table does as it takes one.
        ///
        /// **At least, so that a caller naming one slot at a time need not know the table's size.**
        /// A debt is owed a row at a time and grows to that row; a table hands its whole size in.
        /// Neither shrinks the bytes, and a table emptied is exactly when the next is about to be
        /// filled.
        void grow(std::size_t count)
        {
            if (count > mFlags.size())
                mFlags.resize(count, 0);
        }

        /// Puts `slot` in the set, once however many times it is named.
        void add(Index slot)
        {
            assert(slot < mFlags.size() && "a slot the table has not grown to");
            if (mFlags[slot] != 0)
                return;

            mFlags[slot] = 1;
            mSlots.push_back(slot);
        }

        /// The same, for a caller that is told one slot at a time and never sees the table.
        ///
        /// **Because "grow before add" is a rule, and a rule is what this type exists to hold.** A
        /// debt is named a row rather than a row count, so its alternative is a `grow` beside every
        /// `add` — which the assert above only catches once somebody has already forgotten it.
        void addMakingRoom(Index slot)
        {
            grow(std::size_t{ slot } + 1);
            add(slot);
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

        /// Takes out every slot at or past `count`, and holds room for exactly that many.
        ///
        /// **`grow`'s counterpart, for a table that shrank.** A slot above the new end names a row
        /// that is gone, so a reader walking the list indexes past it — and the assert in `add`
        /// accepts such a slot, because the flags still stretch that far. So the flags shrink here
        /// where `grow` never shrinks them, and the bytes behind them stay where they are.
        void shrinkTo(std::size_t count);

        /// Whether `slot` is in the set. **Answers while a `remove` is outstanding**, where
        /// `getSlots` will not: the flags are exact from the moment a slot is taken out, and it is
        /// the list that has to wait for `compact`.
        bool has(Index slot) const { return slot < mFlags.size() && mFlags[slot] != 0; }

        std::span<const Index> getSlots() const
        {
            assert(!mStale && "the list was read between a remove and the compact that settles it");
            return mSlots;
        }

        bool empty() const { return getSlots().empty(); }

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
