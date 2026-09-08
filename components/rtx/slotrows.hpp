#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "index.hpp"

namespace Rtx
{
    /// A table of fixed-size rows: the rows, the slots nothing stands in, and the sweep.
    ///
    /// **One type, because six tables held the same three rules.** Each kept a row vector beside a
    /// free list, and two of them spelled the taking out by hand because they carry arrays parallel
    /// to the rows that a shared helper had no way to reach. `take`'s growth hook is what reaches
    /// them.
    ///
    /// **A slot is never moved and never closed up.** A mesh index names a bottom-level
    /// acceleration structure and a texture index is what a material points at, so a dropped row
    /// leaves a hole and the next arrival takes it over.
    ///
    /// **And the hole it takes is the lowest, never the last one freed.** Every row is the same
    /// size, so any hole would hold the row — but which one it is decides what a run draws.
    /// `Rtx::Identity` hashes by address, so a sweep retires in whatever order the allocator left
    /// its map in, and a list taken from the back then hands the same live set different slots in
    /// two processes. The lowest makes the answer a function of what is standing rather than of the
    /// order the dead left in. Measured on `one-cell-walk`: the `materials` and `textures` columns
    /// differed from frame 2 on 89 frames of 90.
    ///
    /// **What a freed row holds is the table's business and not this one's.** A mesh row keeps the
    /// offsets its last tenant left, because a backend walks every slot and reads a count of
    /// nothing; a material row is emptied outright. So `free` puts the slot back and writes nothing,
    /// and `sweep` hands each row to the caller before it goes.
    template <class Row>
    class SlotRows
    {
    public:
        std::size_t size() const { return mRows.size(); }

        /// How many slots hold a row, which is what a sweep compares its survivors against.
        std::size_t getLiveCount() const { return mRows.size() - mFree.size(); }

        std::span<const Row> getRows() const { return mRows; }

        const Row& at(Index slot) const
        {
            assert(slot < mRows.size());
            return mRows[slot];
        }

        Row& at(Index slot)
        {
            assert(slot < mRows.size());
            return mRows[slot];
        }

        /// Puts `row` in a free slot, or in a new one.
        ///
        /// @param grew called with the table's new length wherever the table grew, so a caller
        ///        holding arrays parallel to this one follows in the same call. `TextureTable`
        ///        keeps two names beside its rows, and `PlacementTable` keeps a previous transform.
        template <class Grew>
        Index take(const Row& row, Grew grew)
        {
            if (mFree.empty())
            {
                mRows.push_back(row);
                grew(mRows.size());

                return static_cast<Index>(mRows.size() - 1);
            }

            const Index index = takeLowest(mFree);
            mRows[index] = row;

            return index;
        }

        Index take(const Row& row)
        {
            return take(row, [](std::size_t) {});
        }

        /// Puts `slot` back. What its row now holds is the caller's to have decided.
        void free(Index slot)
        {
            assert(slot < mRows.size());
            giveBack(mFree, slot);
        }

        /// Notes every slot a sweep must not free, and says how many distinct ones `keep` named.
        ///
        /// **Apart from `sweep`, because a scene marks two tables and frees neither where both came
        /// back whole.**
        ///
        /// **Distinct, which is what lets duplicates and any order be fine.** A caller compares this
        /// against how many rows are live to decide whether anything died, so a span measured by its
        /// length would read as a larger set than it is the moment a caller named one row twice.
        std::size_t mark(std::span<const Index> keep)
        {
            // Cleared before it is grown, so the fill reaches every row rather than only the rows
            // past the length the last sweep left. A table that never sweeps never allocates it.
            mKept.clear();
            mKept.resize(mRows.size(), 0);

            std::size_t distinct = 0;
            for (const Index index : keep)
            {
                assert(index < mRows.size());
                distinct += mKept[index] == 0 ? 1 : 0;
                mKept[index] = 1;
            }

            // A slot already free is one nothing may free again.
            for (const Index index : mFree)
                mKept[index] = 1;

            return distinct;
        }

        /// Frees every slot the last `mark` did not name, and says how many that was.
        ///
        /// @param release `void(Index, Row&)`, called before each row goes. What the row named is
        ///        given back there — a mesh's runs, a material's textures — because only the table
        ///        that owns the row knows what it owns.
        template <class Release>
        std::size_t sweep(Release release)
        {
            assert(mKept.size() == mRows.size() && "a sweep with no mark in front of it");

            std::size_t freed = 0;
            for (Index index = 0; index < mRows.size(); ++index)
            {
                if (mKept[index] != 0)
                    continue;

                release(index, mRows[index]);
                giveBack(mFree, index);
                ++freed;
            }

            return freed;
        }

    private:
        /// The lowest slot on `free`, taken off it.
        static Index takeLowest(std::vector<Index>& free)
        {
            assert(!free.empty() && "a slot taken from a list with none on it");

            std::pop_heap(free.begin(), free.end(), std::greater<>());
            const Index index = free.back();
            free.pop_back();

            return index;
        }

        /// Puts `index` back, so the next `takeLowest` may answer with it.
        ///
        /// **The only way onto the list.** A slot pushed onto it by anything else leaves it no heap,
        /// and the pop above is then undefined and answers with whatever the top happens to be.
        static void giveBack(std::vector<Index>& free, const Index index)
        {
            free.push_back(index);
            std::push_heap(free.begin(), free.end(), std::greater<>());
        }

        std::vector<Row> mRows;

        /// A min-heap of the slots nothing stands in.
        std::vector<Index> mFree;

        /// Which slots the last `mark` named, one flag per row.
        ///
        /// **Held rather than made, because a sweep runs on the frame a cell left** — the frame
        /// that is already giving thousands of runs back to the allocators, and the last one that
        /// should also be sizing a buffer to the whole table.
        std::vector<std::uint8_t> mKept;
    };
}
