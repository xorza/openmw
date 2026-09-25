#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "runs.hpp"
#include "stepped.hpp"

namespace Rtx
{
    /// The slots of a table that nothing stands in. The lowest is what a take answers with, never
    /// the last one freed: `Rtx::Identity` hashes by address, so a sweep retires in whatever order
    /// the allocator left its map in, and a list taken from the back would hand the same live set
    /// different slots in two processes, which `rtx debug repeat` catches. Its own type
    /// because `GuiTextures` and the view scenes hold rows `SlotRows` cannot — a `unique_ptr` is
    /// move-only and `SlotRows::take` copies. A byte per slot says whether it is on the heap, so
    /// a slot freed twice is an assert here and not two entries the heap hands out as two slots.
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
            mIsFree[index] = 0;

            return index;
        }

        /// Puts `slot` back, so the next `take` may answer with it. The only way onto the list, or
        /// it is no heap and the pop above answers with whatever the top happens to be.
        void free(Index slot)
        {
            // Grown here and not by the table, as `SlotSet::addMakingRoom` grows: the first free of
            // a slot is after the growth that made it, and a free is not the frame path's busy part.
            if (slot >= mIsFree.size())
                mIsFree.resize(std::size_t{ slot } + 1, 0);
            assert(mIsFree[slot] == 0 && "a slot freed twice");

            mIsFree[slot] = 1;
            mFree.push_back(slot);
            std::push_heap(mFree.begin(), mFree.end(), std::greater<>());
        }

        /// Whether `slot` is on the list — what a caller asks before it gives one back.
        bool isFree(Index slot) const { return slot < mIsFree.size() && mIsFree[slot] != 0; }

        /// How many slots stand empty, which a table subtracts from its length to count what lives.
        std::size_t size() const { return mFree.size(); }

        /// Every empty slot, in the heap's own order. For a mark, which walks all of them rather
        /// than searching per row.
        std::span<const Index> getSlots() const { return mFree; }

    private:
        /// A min-heap of the slots nothing stands in.
        std::vector<Index> mFree;

        /// A byte per slot ever freed, set for exactly the slots the heap holds.
        std::vector<std::uint8_t> mIsFree;
    };

    /// The slots of one table that something is true of, in the order they were named, each once.
    /// The list is what a frame walks and the byte is what keeps a slot named twice from appearing
    /// twice without searching the list — N²/2 comparisons for the N movers of a crowded cell.
    /// Kept together, because apart they fall out of step in the one direction nothing catches.
    class SlotSet
    {
    public:
        /// Makes room for at least `count` slots, which a table does as it takes one. Never shrinks
        /// the bytes: a table emptied is exactly when the next is about to be filled.
        void grow(std::size_t count)
        {
            if (count > mFlags.size())
                mFlags.resize(count);
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
        void addMakingRoom(Index slot)
        {
            grow(std::size_t{ slot } + 1);
            add(slot);
        }

        /// Takes `slot` out. The list is left holding it until `compact` runs, because a sweep
        /// takes thousands back and erasing from the middle is that many moves apiece; `getSlots`
        /// will not answer while one is outstanding.
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

        /// Takes out every slot at or past `count`, and holds room for exactly that many — `grow`'s
        /// counterpart, for a table that shrank, so the assert in `add` stops accepting a slot that
        /// names a row that is gone.
        void shrinkTo(std::size_t count);

        /// Whether `slot` is in the set. Answers while a `remove` is outstanding, where `getSlots`
        /// will not: the flags are exact from the moment a slot is taken out.
        bool has(Index slot) const { return slot < mFlags.size() && mFlags[slot] != 0; }

        std::span<const Index> getSlots() const
        {
            assert(!mStale && "the list was read between a remove and the compact that settles it");
            return mSlots;
        }

        bool empty() const { return getSlots().empty(); }

        /// Empties the set. Only the slots in it are put back, rather than the whole table: a
        /// worldspace is thousands of slots and what a frame names is tens.
        void clear();

    private:
        std::vector<Index> mSlots;

        /// A byte per slot of the table beside it, set for exactly the slots `mSlots` names —
        /// except between a `remove` and the `compact` that settles it.
        std::vector<std::uint8_t> mFlags;

        bool mStale = false;
    };

    /// What has happened to one slot of a table since the last `clearArrivals`.
    enum class SlotNews : std::uint8_t
    {
        Arrived,
        Freed,
    };

    /// Which slots of one table arrived and which were given back, since a frame last read them.
    /// Two sets, and a slot stands in at most one of them: the last word wins, so a slot that
    /// arrived and went inside one frame is reported gone, and one that went and was taken over
    /// is reported arrived. A backend then releases a slot it never built and builds into one it
    /// never released — both of which it takes as nothing, and the hand-over relies on it. A
    /// backend comparing table sizes could not tell, and a slot taken over in place would tell it
    /// nothing at all.
    class SlotChanges
    {
    public:
        /// Makes room for at least `count` slots, which a table does as it takes one.
        void grow(std::size_t count)
        {
            mArrived.grow(count);
            mFreed.grow(count);
        }

        /// Records `slot` as having arrived or gone. The last word wins and the earlier one is
        /// taken back, because what a reader has to know is where the slot stands at the end of the
        /// frame.
        void note(Index slot, SlotNews what);

        std::span<const Index> getArrived() const { return mArrived.getSlots(); }
        std::span<const Index> getFreed() const { return mFreed.getSlots(); }

        /// Empties both sets.
        void clearArrivals()
        {
            mArrived.clear();
            mFreed.clear();
        }

    private:
        SlotSet mArrived;
        SlotSet mFreed;
    };

    /// A table of fixed-size rows: the rows, the slots nothing stands in, what holds each, and the
    /// sweep. A slot is never moved and never closed up — a mesh index names a bottom-level
    /// acceleration structure and a texture index is what a material points at — so a dropped row
    /// leaves a hole and the next arrival takes the lowest one (`SlotPool`). The hold count lives
    /// here and the table decides what a count of nought means: a texture is named by materials, a
    /// rig by the meshes on it, a ground row by the residency that stood it. What a freed row holds
    /// is the table's business too — a mesh row keeps its last tenant's offsets because a backend
    /// walks every slot, a material row is emptied — so `free` writes nothing and `sweep` hands
    /// each row to the caller before it goes.
    template <class Row>
    class SlotRows
    {
    public:
        std::size_t size() const { return mRows.size(); }

        /// How many slots hold a row, which is what a sweep compares its survivors against.
        std::size_t getLiveCount() const { return mRows.size() - mFree.size(); }

        std::span<const Row> getRows() const { return mRows; }

        /// Whether `slot` holds a row: what an index into this table has to name to mean anything,
        /// and what a placement's mesh or material is checked against. A byte per row, kept so the
        /// question is one read and not a walk of the free list.
        bool isLive(Index slot) const
        {
            assert(slot < mRows.size());
            return mLive[slot] != 0;
        }

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

        /// Puts `row` in a free slot, or in a new one. The slot arrives with no holds. Everything
        /// a table knows about a slot is in the row, so nothing beside the rows has to follow a
        /// growth. By value and moved in, so a row that owns a name is built once.
        Index take(Row row)
        {
            mMark.step(Mark::Stale, Mark::Stale, Mark::Fresh);

            const Index index = mFree.take();
            if (index == sNoIndex)
            {
                mRows.push_back(std::move(row));
                mHolds.push_back(0);
                mLive.push_back(1);

                return static_cast<Index>(mRows.size() - 1);
            }

            assert(mHolds[index] == 0 && "a free slot something still holds");
            mRows[index] = std::move(row);
            mLive[index] = 1;

            return index;
        }

        /// Puts `slot` back. What its row now holds is the caller's to have decided.
        void free(Index slot)
        {
            mMark.step(Mark::Stale, Mark::Stale, Mark::Fresh);

            assert(slot < mRows.size());
            assert(mHolds[slot] == 0 && "a slot freed while something holds it");
            assert(mLive[slot] != 0 && "a slot freed twice");
            mLive[slot] = 0;
            mFree.free(slot);
        }

        void hold(Index slot)
        {
            assert(slot < mRows.size());
            ++mHolds[slot];
        }

        /// Counts one holder off `slot`, and says whether that was the last. What the last one
        /// means is the table's to decide: a texture frees the slot on the spot, a mesh or a
        /// material row waits for the sweep `hasDroppedHolds` says it owes.
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
        /// a hold keeps. Apart from `sweep`, because a scene marks two tables and frees neither
        /// where both came back whole. Distinct, because a caller compares this against how many
        /// rows are live, and a span measured by its length would miscount a row named twice.
        std::size_t mark(std::span<const Index> keep)
        {
            mMark.step(Mark::Fresh, Mark::Stale, Mark::Fresh);

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

        /// Frees every slot the last `mark` did not name, and says how many that was. Once per
        /// mark: a second sweep on the same mark freed every row again, and a take or a free
        /// between the two makes the mark describe rows that are not there — both are the step.
        ///
        /// @param release `void(Index, Row&)`, called before each row goes. What the row named is
        ///        given back there — a mesh's runs, a material's textures — because only the table
        ///        that owns the row knows what it owns.
        template <class Release>
        std::size_t sweep(Release release)
        {
            mMark.step(Mark::Stale, Mark::Fresh);

            std::size_t freed = 0;
            for (Index index = 0; index < mRows.size(); ++index)
            {
                if (mKept[index] != 0)
                    continue;

                release(index, mRows[index]);
                mLive[index] = 0;
                mFree.free(index);
                ++freed;
            }

            return freed;
        }

    private:
        std::vector<Row> mRows;

        /// How many things hold each row, parallel to the rows.
        std::vector<std::uint32_t> mHolds;

        /// Whether each slot holds a row, parallel to the rows — `isLive`.
        std::vector<std::uint8_t> mLive;

        SlotPool mFree;

        /// Which slots the last `mark` named, one flag per row. Held rather than made, because a
        /// sweep runs on the frame a cell left, which is busy enough already.
        std::vector<std::uint8_t> mKept;

        /// Whether `mKept` describes the rows as they stand: fresh from a mark until the sweep that
        /// consumes it or the take or free that changes what it describes.
        enum class Mark
        {
            Stale,
            Fresh,
        };

        Stepped<Mark> mMark{ Mark::Stale };

        bool mDroppedHolds = false;
    };

    /// The half of `SlotRows` a reader and a sweep use, for a table whose rows go in through its
    /// own `add` and out through its own `sweep`: `take`, `at`, `free` and the raw sweep stay with
    /// the table, which alone knows what a freed row holds and what it gives back. One base and
    /// not the eight forwarders written per table, which were one policy twice.
    template <class Row>
    class HeldRows
    {
    public:
        std::size_t size() const { return mRows.size(); }
        std::size_t getLiveCount() const { return mRows.getLiveCount(); }
        bool isLive(Index slot) const { return mRows.isLive(slot); }
        std::span<const Row> getRows() const { return mRows.getRows(); }
        void hold(Index slot) { mRows.hold(slot); }
        bool drop(Index slot) { return mRows.drop(slot); }
        bool hasDroppedHolds() const { return mRows.hasDroppedHolds(); }
        std::size_t mark(std::span<const Index> keep) { return mRows.mark(keep); }

    protected:
        SlotRows<Row> mRows;
    };

    /// Orders rows by the key `KeyOf` takes from each, and takes a bare key on either side, so a
    /// `boost::container::flat_set` of rows is searched by the key alone. One type for both the
    /// insertion and the search, so no reader can disagree with another about whether a key is
    /// held — which is what a `std::lower_bound` at each site with a comparator of its own could.
    ///
    /// @tparam Key what a row is ordered by, a type distinct from the row's.
    /// @tparam KeyOf what a row's key is, as a stateless callable taking the row.
    template <class Key, class KeyOf>
    struct KeyedLess
    {
        using is_transparent = void;

        template <class Left, class Right>
        bool operator()(const Left& left, const Right& right) const
        {
            return keyOf(left) < keyOf(right);
        }

    private:
        /// A key as it is, and a row by what `KeyOf` takes from it. By convertibility and not by
        /// overload, because a lookup by a pointer to non-const is a key too, and an overload on
        /// `const Key&` would lose to the row template's exact match there.
        template <class T>
        static Key keyOf(const T& value)
        {
            if constexpr (std::is_convertible_v<const T&, Key>)
                return value;
            else
                return KeyOf{}(value);
        }
    };
}
