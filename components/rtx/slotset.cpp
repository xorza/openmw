#include "slotset.hpp"

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
}
