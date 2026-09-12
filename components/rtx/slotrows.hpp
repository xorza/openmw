#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "index.hpp"
#include "slotpool.hpp"

namespace Rtx
{
    /// A table of fixed-size rows: the rows, the slots nothing stands in, what holds each, and the
    /// sweep.
    ///
    /// **One type, because six tables held the same three rules.** Each kept a row vector beside a
    /// free list, and two of them spelled the taking out by hand because they carry arrays parallel
    /// to the rows that a shared helper had no way to reach. `take`'s growth hook is what reaches
    /// them.
    ///
    /// **And one hold count.** A texture is named by materials, a rig by the meshes on it, a
    /// ground row by the residency that stood it; each is a count beside the row that says whether
    /// the row may go. The count lives here, the table decides what a count of nought means.
    ///
    /// **A slot is never moved and never closed up.** A mesh index names a bottom-level
    /// acceleration structure and a texture index is what a material points at, so a dropped row
    /// leaves a hole and the next arrival takes it over.
    ///
    /// **And the hole it takes is the lowest, never the last one freed.** `SlotPool` is that rule
    /// and says why.
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

        /// Puts `row` in a free slot, or in a new one. The slot arrives with no holds.
        ///
        /// @param grew called with the table's new length wherever the table grew, so a caller
        ///        holding arrays parallel to this one follows in the same call. `TextureTable`
        ///        keeps two names beside its rows, and `PlacementTable` keeps a previous transform.
        template <class Grew>
        Index take(const Row& row, Grew grew)
        {
            const Index index = mFree.take();
            if (index == sNoIndex)
            {
                mRows.push_back(row);
                mHolds.push_back(0);
                grew(mRows.size());

                return static_cast<Index>(mRows.size() - 1);
            }

            assert(mHolds[index] == 0 && "a free slot something still holds");
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
            assert(mHolds[slot] == 0 && "a slot freed while something holds it");
            mFree.free(slot);
        }

        /// Counts one more holder of `slot`.
        void hold(Index slot)
        {
            assert(slot < mRows.size());
            ++mHolds[slot];
        }

        /// Counts one holder off `slot`, and says whether that was the last.
        ///
        /// **What the last one means is the table's to decide**: a texture frees the slot on the
        /// spot, a rig with it, and a mesh or a material row waits for the sweep — which is what
        /// `hasDroppedHolds` tells the caller it owes.
        bool drop(Index slot)
        {
            assert(slot < mRows.size());
            assert(mHolds[slot] > 0 && "a slot given back more often than it was held");

            if (--mHolds[slot] != 0)
                return false;

            mDroppedHolds = true;
            return true;
        }

        std::uint32_t getHolds(Index slot) const
        {
            assert(slot < mRows.size());
            return mHolds[slot];
        }

        /// Whether a hold went to nought since the last `mark`, so that a sweep gated on some other
        /// count still runs for it.
        bool hasDroppedHolds() const { return mDroppedHolds; }

        /// Notes every slot a sweep must not free, and says how many distinct ones `keep` named or
        /// a hold keeps.
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

            // A held row is a survivor whether or not anything named it.
            for (Index index = 0; index < mRows.size(); ++index)
                if (mHolds[index] != 0 && mKept[index] == 0)
                {
                    mKept[index] = 1;
                    ++distinct;
                }

            // A slot already free is one nothing may free again.
            for (const Index index : mFree.getSlots())
                mKept[index] = 1;

            mDroppedHolds = false;
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
                mFree.free(index);
                ++freed;
            }

            return freed;
        }

    private:
        std::vector<Row> mRows;

        /// How many things hold each row, parallel to the rows.
        std::vector<std::uint32_t> mHolds;

        /// The slots nothing stands in.
        SlotPool mFree;

        /// Which slots the last `mark` named, one flag per row.
        ///
        /// **Held rather than made, because a sweep runs on the frame a cell left** — the frame
        /// that is already giving thousands of runs back to the allocators, and the last one that
        /// should also be sizing a buffer to the whole table.
        std::vector<std::uint8_t> mKept;

        bool mDroppedHolds = false;
    };
}
