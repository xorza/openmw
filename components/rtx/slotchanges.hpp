#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "index.hpp"
#include "slotset.hpp"

namespace Rtx
{
    /// What has happened to one slot of a table since the last `clearArrivals`.
    enum class SlotNews : std::uint8_t
    {
        Arrived,
        Freed,
    };

    /// Which slots of one table arrived and which were given back, since a frame last read them.
    ///
    /// **Two sets, and a slot stands in at most one of them.** A slot that arrives and goes inside
    /// one frame belongs to neither, and one named twice belongs to its own once — neither of which
    /// a caller pushing onto two vectors can promise. Without this a backend would have to work the
    /// same answer out by comparing table sizes it does not keep, and a slot taken over in place
    /// would tell it nothing at all.
    ///
    /// **One of these per table**, which is what lets a table be asked what changed rather than
    /// leaving a set of vectors for somebody else to hold in step with it.
    class SlotChanges
    {
    public:
        /// Makes room for at least `count` slots, which a table does as it takes one.
        void grow(std::size_t count)
        {
            mArrived.grow(count);
            mFreed.grow(count);
        }

        /// Records `slot` as having arrived or gone.
        ///
        /// **The last word wins, and the earlier one is taken back.** A slot that arrived and is now
        /// freed leaves the arrivals rather than appearing in both, because what a reader has to
        /// know is where the slot stands at the end of the frame.
        void note(Index slot, SlotNews what);

        std::span<const Index> getArrived() const { return mArrived.getSlots(); }
        std::span<const Index> getFreed() const { return mFreed.getSlots(); }

        /// Empties both sets.
        ///
        /// **Only the slots that have news are reset**, rather than the whole table: a worldspace is
        /// thousands of slots and what a frame changes is tens.
        void clearArrivals()
        {
            mArrived.clear();
            mFreed.clear();
        }

    private:
        SlotSet mArrived;
        SlotSet mFreed;
    };
}
