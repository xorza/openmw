#pragma once

#include <cstdint>
#include <span>

namespace Rtx
{
    /// A run inside a buffer: where it starts, and how many elements it holds.
    ///
    /// **Its own header because nearly every table holds one.** A mesh's vertices, a material's
    /// layers, a layer's mask weights and an emitter's sprites are all runs, so the struct that
    /// names one must not have to include the allocator that hands them out. `index.hpp` is here
    /// for the same reason.
    struct Run
    {
        std::uint32_t mOffset = 0;
        std::uint32_t mCount = 0;

        std::uint32_t getEnd() const { return mOffset + mCount; }
        bool empty() const { return mCount == 0; }

        /// The part of `all` this run names.
        ///
        /// **One call, because the two halves are only ever read together.** Spelled out, each
        /// reader pairs an offset with a count by hand — and a reader that paired one run's offset
        /// with another's count would index a table that exists, by a length that is not its own.
        template <class T>
        std::span<T> in(std::span<T> all) const
        {
            return all.subspan(mOffset, mCount);
        }

        bool operator==(const Run& other) const = default;
    };
}
