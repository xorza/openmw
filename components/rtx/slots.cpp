#include "slots.hpp"

namespace Rtx
{
    void SlotSet::compact()
    {
        if (!mStale)
            return;

        std::erase_if(mSlots, [this](const Index slot) { return mFlags[slot] == 0; });
        mStale = false;
    }

    void SlotSet::shrinkTo(const std::size_t count)
    {
        if (count >= mFlags.size())
            return;

        std::erase_if(mSlots, [count](const Index slot) { return slot >= count; });
        mFlags.resize(count);
    }

    void SlotSet::clear()
    {
        for (const Index slot : mSlots)
            mFlags[slot] = 0;

        mSlots.clear();
        mStale = false;
    }

    void SlotChanges::note(const Index slot, const SlotNews what)
    {
        const bool arriving = what == SlotNews::Arrived;
        SlotSet& taking = arriving ? mArrived : mFreed;
        SlotSet& giving = arriving ? mFreed : mArrived;

        // Compacted here rather than left for the reader, because the other set is asked for
        // straight after this returns and a slot changes its news rarely enough to pay a pass then.
        giving.remove(slot);
        giving.compact();

        taking.add(slot);
    }
}
