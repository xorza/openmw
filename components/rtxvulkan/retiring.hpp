#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace Rtx
{
    /// What a submit may still be reading, each entry with the value of the submit that last
    /// does, in the order held — which is stamp order, because the stamp is the next submit's
    /// value and the queue takes submits in order — so what a wait lets go of is a prefix. The
    /// graveyard keeps one per kind of device object and the pool one for its command buffers.
    template <class T>
    class Retiring
    {
    public:
        void hold(const std::uint64_t until, T&& what) { mHeld.push_back(Held{ until, std::move(what) }); }

        /// Hands `let` every entry stamped at or below `finished`, in order, and forgets it.
        template <class Let>
        void releaseThrough(const std::uint64_t finished, Let&& let)
        {
            const auto kept = std::find_if(
                mHeld.begin(), mHeld.end(), [finished](const Held& each) { return each.mUntil > finished; });
            for (auto at = mHeld.begin(); at != kept; ++at)
                let(at->mObject);

            mHeld.erase(mHeld.begin(), kept);
        }

        std::size_t size() const { return mHeld.size(); }

    private:
        struct Held
        {
            std::uint64_t mUntil = 0;
            T mObject;
        };

        // Cleared and refilled, never freed: a frame path does not allocate, and what a frame
        // holds settles at the busiest frame so far.
        std::vector<Held> mHeld;
    };
}
