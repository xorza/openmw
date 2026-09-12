#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <utility>
#include <vector>

#include "runs.hpp"

namespace Rtx
{
    /// The slots of a table that nothing stands in. The lowest is what a take answers with, never
    /// the last one freed: `Rtx::Identity` hashes by address, so a sweep retires in whatever order
    /// the allocator left its map in, and a list taken from the back would hand the same live set
    /// different slots in two processes, which `apps/rtxtool/repeatable.sh` catches. Its own type
    /// because `GuiTextures` and the view scenes hold rows `SlotRows` cannot — a `unique_ptr` is
    /// move-only and `SlotRows::take` copies.
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

        /// Puts `slot` back, so the next `take` may answer with it. The only way onto the list, or
        /// it is no heap and the pop above answers with whatever the top happens to be.
        void free(Index slot)
        {
            mFree.push_back(slot);
            std::push_heap(mFree.begin(), mFree.end(), std::greater<>());
        }

        /// How many slots stand empty, which a table subtracts from its length to count what lives.
        std::size_t size() const { return mFree.size(); }

        /// Every empty slot, in the heap's own order. For a mark, which walks all of them rather
        /// than searching per row.
        std::span<const Index> getSlots() const { return mFree; }

    private:
        /// A min-heap of the slots nothing stands in.
        std::vector<Index> mFree;
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

        /// The same, for a caller that is told one slot at a time and never sees the table. The
        /// pragma is a GCC 16 false positive: with `NDEBUG` the optimiser inlines the `resize` and
        /// reports its uninitialised move as writing past a region it deduced from nothing, where
        /// `mFlags[slot]` is in range by the `grow` on the line above it.
        void addMakingRoom(Index slot)
        {
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#endif
            grow(std::size_t{ slot } + 1);
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
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
        bool has(Index slot) const
        {
            return slot < mFlags.size() && mFlags[slot] != 0;
        }

        std::span<const Index> getSlots() const
        {
            assert(!mStale && "the list was read between a remove and the compact that settles it");
            return mSlots;
        }

        bool empty() const
        {
            return getSlots().empty();
        }

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
    /// Two sets, and a slot stands in at most one of them: a slot that arrives and goes inside one
    /// frame belongs to neither. A backend comparing table sizes could not tell, and a slot taken
    /// over in place would tell it nothing at all.
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
    /// each row to the caller before it goes. `take`'s growth hook reaches the arrays a table
    /// keeps parallel to its rows.
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

        SlotPool mFree;

        /// Which slots the last `mark` named, one flag per row. Held rather than made, because a
        /// sweep runs on the frame a cell left, which is busy enough already.
        std::vector<std::uint8_t> mKept;

        bool mDroppedHolds = false;
    };

    /// Rows kept in the order of a key taken from each, found by binary search. A type rather than
    /// a `std::lower_bound` at each site, because a search whose comparator disagreed with the
    /// insertion finds nothing and says nothing. Not a map: what these hold is walked in order every
    /// frame, and the walk is the same walk on every machine. A row's address is not stable, so a
    /// caller holds the key; `Spares` is the type for the other case.
    ///
    /// @tparam KeyOf what a row's key is, as a stateless callable taking the row.
    template <class Row, class Key, class KeyOf>
    class SortedRows
    {
    public:
        using Rows = std::vector<Row>;
        using iterator = typename Rows::iterator;
        using const_iterator = typename Rows::const_iterator;

        /// The row `key` names, or null where nothing holds it.
        const Row* find(const Key& key) const
        {
            const const_iterator at = lowerBound(key);
            return at != mRows.end() && !(key < KeyOf{}(*at)) ? &*at : nullptr;
        }

        Row* find(const Key& key) { return const_cast<Row*>(std::as_const(*this).find(key)); }

        bool contains(const Key& key) const { return find(key) != nullptr; }

        /// The row `key` names, for a caller whose contract is that there is one. A reference,
        /// because callers that asserted `find`'s pointer and dereferenced it left a release build
        /// with a path that reads through nought.
        const Row& at(const Key& key) const
        {
            const const_iterator found = lowerBound(key);
            assert(found != mRows.end() && !(key < KeyOf{}(*found)) && "a key read that nothing holds");

            return *found;
        }

        Row& at(const Key& key) { return const_cast<Row&>(std::as_const(*this).at(key)); }

        /// The row `key` names, inserting what `make` answers where nothing holds it. One search
        /// either way: a cell arriving asks it once per model it names.
        template <class Make>
        Row& findOrInsert(const Key& key, Make make)
        {
            const iterator at = lowerBound(key);
            if (at != mRows.end() && !(key < KeyOf{}(*at)))
                return *at;

            return *mRows.insert(at, make());
        }

        /// Puts `row` where its own key says. Asserts where something already holds that key.
        Row& insert(Row row)
        {
            const iterator at = lowerBound(KeyOf{}(row));
            assert((at == mRows.end() || KeyOf{}(row) < KeyOf{}(*at)) && "a key inserted twice");

            return *mRows.insert(at, std::move(row));
        }

        /// Takes the row `key` names away and answers it. Asserts where nothing holds it.
        Row take(const Key& key)
        {
            const iterator at = lowerBound(key);
            assert(at != mRows.end() && !(key < KeyOf{}(*at)) && "a key taken that nothing holds");

            Row taken = std::move(*at);
            mRows.erase(at);
            return taken;
        }

        /// Drops the row `key` names. Asserts where nothing holds it.
        void erase(const Key& key)
        {
            const iterator at = lowerBound(key);
            assert(at != mRows.end() && !(key < KeyOf{}(*at)) && "a key dropped that nothing holds");

            mRows.erase(at);
        }

        /// Drops the row at `at` and answers what follows it, for a sweep that walks them all.
        iterator erase(const_iterator at) { return mRows.erase(at); }

        template <class Gone>
        void eraseIf(Gone gone)
        {
            std::erase_if(mRows, gone);
        }

        void clear() { mRows.clear(); }

        std::size_t size() const { return mRows.size(); }

        iterator begin() { return mRows.begin(); }
        iterator end() { return mRows.end(); }

    private:
        /// The first row whose key is not below `key` — the one comparator this type has.
        iterator lowerBound(const Key& key) { return std::lower_bound(mRows.begin(), mRows.end(), key, before); }
        const_iterator lowerBound(const Key& key) const
        {
            return std::lower_bound(mRows.begin(), mRows.end(), key, before);
        }

        static bool before(const Row& row, const Key& wanted) { return KeyOf{}(row) < wanted; }

        Rows mRows;
    };
}
