#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "index.hpp"

namespace Rtx
{
    /// What has happened to one slot of a table since the last `clearArrivals`.
    enum class SlotNews : std::uint8_t
    {
        None,
        Arrived,
        Freed,
    };

    /// Which slots of one table arrived and which were given back, since a frame last read them.
    ///
    /// **Two lists and a byte per slot, and the byte is what keeps the lists exact.** A slot that
    /// arrives and goes inside one frame belongs to neither list, and one named twice belongs to
    /// its own list once — neither of which a caller pushing onto two vectors can promise. Without
    /// it a backend would have to work the same answer out by comparing table sizes it does not
    /// keep, and a slot taken over in place would tell it nothing at all.
    ///
    /// **One of these per table**, which is what lets a table be asked what changed rather than
    /// leaving a set of vectors for somebody else to hold in step with it.
    class SlotChanges
    {
    public:
        /// Grows the per-slot bytes to `count`, which a table does as it takes a slot.
        ///
        /// A resize to the size it already is does not allocate, which is what the frame path pays.
        void grow(std::size_t count) { mNews.resize(count, SlotNews::None); }

        /// Records `slot` as having arrived or gone.
        ///
        /// **The last word wins, and the earlier one is taken back.** A slot that arrived and is now
        /// freed leaves the arrivals rather than appearing in both, because what a reader has to
        /// know is where the slot stands at the end of the frame.
        void note(Index slot, SlotNews what);

        std::span<const Index> getArrived() const { return mArrived; }
        std::span<const Index> getFreed() const { return mFreed; }

        /// Empties both lists and puts their slots back to `None`.
        ///
        /// **Only the slots that have news are reset**, rather than the whole table: a worldspace is
        /// thousands of slots and what a frame changes is tens.
        void clearArrivals();

        /// Drops the bytes as well, for a table that has been emptied.
        void clear();

    private:
        std::vector<SlotNews> mNews;
        std::vector<Index> mArrived;
        std::vector<Index> mFreed;
    };
}
