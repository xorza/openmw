#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Rtx
{
    /// Runs of indices under a dense key, as one list the device reads whole.
    ///
    /// **One list and not two, because the starts and the runs are one fact.** Key `k` owns
    /// `list[list[k]] .. list[list[k + 1]]`: the first `keys + 1` entries say where each run starts,
    /// counted from the front of the list, and the runs follow them. A prefix sum with a trailing
    /// sentinel, so a lookup is two reads and no search however many keys there are, the last run's
    /// end needs no special case, and the device is handed one address rather than two that have to
    /// be kept in step. `LightGrid` bins lamps into cells this way, and the sprite tiles are made
    /// in the same shape on the device by `spritestarts.comp`; `lib/bindings.glsl` reads both by
    /// the same rule.
    ///
    /// **Filled by a counting sort in two passes over the same visits**: `count` each pair once,
    /// `place`, then `put` each pair once in the same order. Runs come out in visiting order.
    ///
    /// **Refilled and never replaced**, because a frame must not go back to the allocator for a list
    /// it already has: both vectors settle at their high-water mark.
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
