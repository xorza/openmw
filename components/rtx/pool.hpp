#pragma once

#include <cassert>
#include <cstddef>
#include <vector>

namespace Rtx
{
    /// Entries reused across arrivals: as deep as the most one arrival ever wanted, and never freed.
    ///
    /// **Refilled rather than emptied**, because what an entry holds is room the heap gave it. An
    /// entry handed out again writes into whatever the last one grew, so an arrival pays for that
    /// room once for the life of the run rather than on the frame a cell lands.
    ///
    /// **`next` and `keep` are two calls because a caller may not want what it built.** A chain
    /// built over a texture that already carried one is empty and is left for the next arrival, so
    /// what says an entry is in use is the caller and not the handing out.
    template <class T>
    class Pool
    {
    public:
        /// The entry after the last kept one, made where the pool has never been this deep.
        T& next()
        {
            if (mKept == mEntries.size())
                mEntries.emplace_back();

            return mEntries[mKept];
        }

        /// Says the entry `next` handed out is in use, and gives back its index.
        std::size_t keep()
        {
            assert(mKept < mEntries.size() && "an entry kept that `next` never handed out");
            return mKept++;
        }

        /// Frees every entry, keeping its room.
        void reset() { mKept = 0; }

        T& operator[](const std::size_t at) { return mEntries[at]; }

    private:
        std::vector<T> mEntries;

        /// How many of them a caller has kept, which is where `next` hands out from.
        std::size_t mKept = 0;
    };
}
