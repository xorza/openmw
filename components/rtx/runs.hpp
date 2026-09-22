#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <vector>

namespace Rtx
{
    /// An index into one of `SceneDesc`'s tables, or `sNoIndex` for "none".
    using Index = std::uint32_t;

    inline constexpr Index sNoIndex = ~Index{ 0 };

    template <class T>
    class BlockedValues;

    /// A run inside a buffer: where it starts, and how many elements it holds. A mesh's vertices,
    /// a material's layers, a layer's mask weights and an emitter's sprites are all runs.
    struct Run
    {
        std::uint32_t mOffset = 0;
        std::uint32_t mCount = 0;

        std::uint32_t getEnd() const { return mOffset + mCount; }
        bool empty() const { return mCount == 0; }

        /// The part of `all` this run names.
        template <class T>
        std::span<T> in(std::span<T> all) const
        {
            return all.subspan(mOffset, mCount);
        }

        template <class T>
        std::span<const T> in(const BlockedValues<T>& all) const
        {
            return all.in(*this);
        }

        bool operator==(const Run& other) const = default;
    };

    /// Hands out runs inside a buffer whose contents never move: closing a hole by moving what is
    /// above it would renumber every offset a shader reads and every device address a
    /// bottom-level acceleration structure was built from. Best fit and not first fit, because the
    /// free list is tens of entries and first fit would spend a cathedral's hole on a crate. Freed
    /// runs that touch are merged.
    class RunAllocator
    {
    public:
        /// @param block a boundary no run may straddle, or zero for a buffer with no such rule —
        ///        what makes appending to a device buffer possible, because a blocked buffer is a
        ///        list of allocations made once and never moved. The tail of a block too short for
        ///        the next run becomes a hole like any other.
        explicit RunAllocator(std::uint32_t block = 0)
            : mBlock(block)
        {
        }

        /// A run of `count` elements, taken from the smallest hole that can hold it, and appended
        /// past the end when none can. `count` must be at least one and, where there is a block
        /// size, no larger than it.
        Run allocate(std::uint32_t count);

        /// Gives a run back. Merged with whatever it touches, and an empty run is not a run. The
        /// run must be one `allocate` returned and must not already be free — asserted, off the
        /// two holes it lands between, since a release finds them anyway.
        void release(Run span);

        /// Forgets every run. The buffer behind it is emptied by whoever owns it.
        void clear();

        /// How far into the buffer anything has ever reached, which is how long the buffer has to
        /// be. Runs given back at the very end shrink it again.
        std::uint32_t getEnd() const { return mEnd; }

        /// How many elements below `getEnd` are in holes.
        std::uint32_t getFree() const;

        /// How many are handed out, which is what a report of a buffer built on this calls live.
        std::uint32_t getUsed() const { return getEnd() - getFree(); }

        // Read by the tests and by nothing else.
        /// How many separate holes those elements are in: a measure of fragmentation, and what says
        /// that releases merged.
        std::size_t getHoleCount() const { return mFree.size(); }

    private:
        /// Where in `hole` a run of `count` can go without straddling a block, or a count of zero
        /// where it cannot go there at all.
        Run place(const Run& hole, std::uint32_t count) const;

        /// The holes, ordered by offset and never touching one another. Ordered so that a release
        /// can find its neighbours, and disjoint-and-separated so that finding them is enough.
        std::vector<Run> mFree;

        std::uint32_t mEnd = 0;
        std::uint32_t mBlock = 0;
    };

    /// The host's side of a buffer in blocks, the shape the device's has: whole blocks that never
    /// move, so a growth allocates one block and copies nothing, and the frame that adopts a cell
    /// pays for its own geometry and not for every vertex before it. Element `at` is the flat
    /// buffer's `at` — `at % block` of block `at / block` — so the offsets a `RunAllocator` of the
    /// same block hands out name the same elements here as on the device, and a run is inside one
    /// block because the allocator never lets one straddle.
    ///
    /// **A block is storage until it is reached**, and `reach` value-initialises only what it
    /// reaches: a scene with a handful of meshes holds a few pages of each block and not the whole
    /// of it, and the holes a block's tail leaves are nought, as a vector's growth left them.
    template <class T>
    class BlockedValues
    {
        static_assert(std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>,
            "a block is raw storage, copied and dropped as bytes");
        static_assert(alignof(T) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__, "a block is aligned as new aligns it");

    public:
        explicit BlockedValues(std::uint32_t block)
            : mBlock(block)
        {
            assert(block > 0 && "a buffer in blocks of nothing");
        }

        /// Makes the buffer `end` long, allocating whole blocks until `end` is inside them. Never
        /// moves a block and never shrinks, like the allocator's buffer it follows.
        void reach(std::uint32_t end)
        {
            if (end <= mEnd)
                return;

            while (static_cast<std::uint64_t>(mBlocks.size()) * mBlock < end)
                mBlocks.push_back(std::unique_ptr<std::byte[]>(new std::byte[std::size_t{ mBlock } * sizeof(T)]));

            for (std::uint32_t at = mEnd; at < end;)
            {
                const std::uint32_t upTo = std::min(end, (at / mBlock + 1) * mBlock);
                std::uninitialized_value_construct_n(blockAt(at / mBlock) + at % mBlock, upTo - at);
                at = upTo;
            }

            mEnd = end;
        }

        /// How far the buffer has been reached, which is how much of it is uploaded.
        std::uint32_t size() const { return mEnd; }

        T& operator[](std::uint32_t at) { return *elementAt(at); }
        const T& operator[](std::uint32_t at) const { return *elementAt(at); }

        /// One run's elements, which lie in one block.
        std::span<T> in(Run run) { return std::span<T>(startOf(run), run.mCount); }
        std::span<const T> in(Run run) const { return std::span<const T>(startOf(run), run.mCount); }

        /// Hands `visit` each block's reached part, in order: the whole buffer, for a reader of all
        /// of it such as the scene digest.
        template <class Visit>
        void forEachBlock(Visit&& visit) const
        {
            for (std::uint32_t at = 0; at < mEnd; at += mBlock)
                visit(std::span<const T>(blockAt(at / mBlock), std::min(mBlock, mEnd - at)));
        }

    private:
        T* blockAt(std::size_t block) const { return std::launder(reinterpret_cast<T*>(mBlocks[block].get())); }

        T* elementAt(std::uint32_t at) const
        {
            assert(at < mEnd);
            return blockAt(at / mBlock) + at % mBlock;
        }

        T* startOf(Run run) const
        {
            if (run.empty())
                return nullptr;

            assert(run.getEnd() <= mEnd && run.mOffset / mBlock == (run.getEnd() - 1) / mBlock
                && "a run past the buffer's end, or across a block");
            return elementAt(run.mOffset);
        }

        std::vector<std::unique_ptr<std::byte[]>> mBlocks;
        std::uint32_t mBlock;
        std::uint32_t mEnd = 0;
    };

    /// A `RunAllocator` and the buffer its runs name, so the buffer reaches the allocator's end
    /// before anything is written into it. Grown and never shrunk. A buffer in blocks is a
    /// `RunAllocator` of the block beside the `BlockedValues` it names, and `DeformerTable`'s bind
    /// runs name a table the backend owns, which is why `RunAllocator` also stands alone.
    template <class T>
    class RunBuffer
    {
    public:
        /// Room for `count` elements, holding whatever its last tenant left, so a caller writes the
        /// whole of it before anything reads it. `allocateZeroed` is the other answer.
        Run allocate(std::uint32_t count)
        {
            const Run run = mRuns.allocate(count);
            if (mValues.size() < mRuns.getEnd())
                mValues.resize(mRuns.getEnd());

            return run;
        }

        /// The same room, holding `values`.
        Run allocate(std::span<const T> values)
        {
            assert(!values.empty() && "a run of nothing is not a run");

            const Run run = allocate(static_cast<std::uint32_t>(values.size()));
            std::copy(values.begin(), values.end(), mValues.begin() + run.mOffset);

            return run;
        }

        /// The same room, value-initialised. A pose nothing can equal, which is what
        /// `DeformerTable::stand` hands a mesh that has not been posed yet.
        Run allocateZeroed(std::uint32_t count)
        {
            const Run run = allocate(count);
            std::fill_n(mValues.begin() + run.mOffset, count, T{});

            return run;
        }

        /// Gives a run back. Merged with whatever it touches, and the buffer keeps its room.
        void release(Run run) { mRuns.release(run); }

        std::span<const T> getAll() const { return mValues; }

        /// One run's elements, for a caller writing into what it took.
        std::span<T> in(Run run) { return run.in(std::span<T>(mValues)); }

        /// How far into the buffer anything has ever reached, which is how much of it is uploaded.
        std::uint32_t getEnd() const { return mRuns.getEnd(); }

        std::uint32_t getUsed() const { return mRuns.getUsed(); }

        // Read by the tests and by nothing else.
        std::size_t getHoleCount() const { return mRuns.getHoleCount(); }

    private:
        RunAllocator mRuns;
        std::vector<T> mValues;
    };

    /// Runs of indices under a dense key, as one list the device reads whole. Key `k` owns
    /// `list[list[k]] .. list[list[k + 1]]`: the first `keys + 1` entries are a prefix sum with a
    /// trailing sentinel, and the runs follow. `LightGrid` bins lamps this way, `spritestarts.comp`
    /// makes the sprite tiles in the same shape, and `lib/bindings.glsl` reads both by one rule.
    /// Filled by a counting sort: `count` each pair once, `place`, then `put` each pair in the same
    /// order. Refilled and never replaced.
    class RunList
    {
    public:
        /// Forgets every run and starts counting for `keys` keys.
        void start(std::size_t keys)
        {
            mList.assign(keys + 1, 0);
            mList[0] = static_cast<std::uint32_t>(keys + 1);
            mCursor.assign(keys, 0);
        }

        /// One more entry under `key`, between `start` and `place`.
        void count(std::size_t key) { ++mList[key + 1]; }

        /// Turns the counts into starts and makes room for the runs, which `put` then fills. A list
        /// nothing was counted into is every start at the head's end and no run at all.
        void place()
        {
            for (std::size_t key = 0; key < mCursor.size(); ++key)
            {
                mList[key + 1] += mList[key];
                mCursor[key] = mList[key];
            }

            mList.resize(mList.back());
        }

        /// Appends `value` to `key`'s run: after `place`, and as often as `count` was called for
        /// `key` and no more.
        void put(std::size_t key, std::uint32_t value)
        {
            assert(mCursor[key] < mList[key + 1] && "a run given more than it was counted for");
            mList[mCursor[key]++] = value;
        }

        /// The whole list, starts and runs, as the device takes it.
        std::span<const std::uint32_t> getWhole() const { return mList; }

        // Read by the tests and by nothing else.
        std::span<const std::uint32_t> getRun(std::size_t key) const
        {
            return std::span<const std::uint32_t>(mList).subspan(mList[key], mList[key + 1] - mList[key]);
        }

        /// How many entries the runs hold between them.
        std::size_t getEntryCount() const { return mList.size() - mList.front(); }

    private:
        std::vector<std::uint32_t> mList;

        /// Where each key's next entry goes while the runs are being filled.
        std::vector<std::uint32_t> mCursor;
    };
}
