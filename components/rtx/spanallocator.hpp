#pragma once

#include <cstdint>
#include <vector>

namespace Rtx
{
    /// A run inside a buffer: where it starts, and how many elements it holds.
    struct Span
    {
        std::uint32_t mOffset = 0;
        std::uint32_t mCount = 0;

        std::uint32_t getEnd() const { return mOffset + mCount; }
        bool empty() const { return mCount == 0; }

        bool operator==(const Span& other) const = default;
    };

    /// Hands out runs inside a buffer whose contents never move.
    ///
    /// **What a table of fixed-size slots cannot do.** Nearly everything the scene holds is a *run*
    /// — a mesh's vertices, a material's layers, a layer's mask weights — and a run is variable
    /// length, so freeing one leaves a hole of a size nothing else is guaranteed to want. Closing
    /// the hole by moving what is above it renumbers everything downstream: the offsets a shader
    /// reads, and the device addresses the bottom-level acceleration structures were built from. So
    /// nothing is ever moved, and this is what remembers where the holes are.
    ///
    /// **Best fit and not first fit.** The free list is what a departing cell left, so it is tens of
    /// entries and walking it costs nothing; first fit would spend a cathedral's hole on a crate and
    /// leave the next cathedral to append. Freed runs that touch are merged, which is what turns a
    /// cell's thousands of small releases back into the one large hole it arrived as.
    class SpanAllocator
    {
    public:
        /// @param block a boundary no run may straddle, or zero for a buffer with no such rule.
        ///
        /// **What makes appending to a device buffer possible.** A buffer that is one allocation
        /// moves when it grows, and everything holding an address into it — every bottom-level
        /// acceleration structure in the world — has to be built again. Blocked, the buffer is a
        /// list of allocations that are made once and never moved, so growing costs one more block
        /// and nothing already in it shifts. The price is that a run has to fit inside a block, and
        /// the tail of a block too short for the next run becomes a hole like any other.
        explicit SpanAllocator(std::uint32_t block = 0)
            : mBlock(block)
        {
        }

        /// A run of `count` elements. Never empty, and never straddling a block.
        ///
        /// Taken from the smallest hole that can hold it, and appended past the end when none can.
        /// `count` must be at least one and, where there is a block size, no larger than it.
        Span allocate(std::uint32_t count);

        /// Gives a run back. Merged with whatever it touches, and an empty run is not a run.
        ///
        /// The run must be one `allocate` returned and must not already be free, which is a contract
        /// on the caller rather than something checked: the free list is walked per allocation and
        /// not per release.
        void release(Span span);

        /// Forgets every run. The buffer behind it is emptied by whoever owns it.
        void clear();

        /// How far into the buffer anything has ever reached, which is how long the buffer has to
        /// be. Runs given back at the very end shrink it again.
        std::uint32_t getEnd() const { return mEnd; }

        /// How many elements below `getEnd` are in holes.
        std::uint32_t getFree() const;

        /// How many separate holes those elements are in. A measure of fragmentation, and what a
        /// test watches to know that releases merged.
        std::size_t getHoleCount() const { return mFree.size(); }

    private:
        /// Where in `hole` a run of `count` can go without straddling a block, or a count of zero
        /// where it cannot go there at all.
        Span place(const Span& hole, std::uint32_t count) const;

        /// The holes, ordered by offset and never touching one another. Ordered so that a release
        /// can find its neighbours, and disjoint-and-separated so that finding them is enough.
        std::vector<Span> mFree;

        std::uint32_t mEnd = 0;
        std::uint32_t mBlock = 0;
    };
}
