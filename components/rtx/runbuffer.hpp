#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "run.hpp"
#include "runallocator.hpp"

namespace Rtx
{
    /// A `RunAllocator` and the buffer its runs name.
    ///
    /// **One type, because a run and the room it names are one fact.** A run is taken from the
    /// allocator, and the buffer has to reach the allocator's end before anything is written into
    /// it — which is not the caller's business, and a caller that forgot it wrote past the end of a
    /// vector.
    ///
    /// **Grown and never shrunk**: a run given back at the very end goes to the allocator, and the
    /// next arrival lands in it rather than in a buffer resized twice.
    ///
    /// **Not what every allocator here wants.** `DeformerTable`'s bind runs hand out offsets into a
    /// table the *backend* owns, and there is no host buffer to grow — so `RunAllocator` stays what
    /// it was and this is what stands beside it wherever there is a buffer.
    template <class T>
    class RunBuffer
    {
    public:
        /// @param block a boundary no run may straddle, or zero. `RunAllocator` says what one is
        ///        for and what it costs.
        explicit RunBuffer(std::uint32_t block = 0)
            : mRuns(block)
        {
        }

        /// Room for `count` elements.
        ///
        /// **What the run holds is whatever its last tenant left**, so a caller writes the whole of
        /// it before anything reads it. `allocateZeroed` is the other answer.
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
        std::size_t getHoleCount() const { return mRuns.getHoleCount(); }

    private:
        RunAllocator mRuns;
        std::vector<T> mValues;
    };
}
