#include "slotset.hpp"

#include <algorithm>

namespace Rtx
{
    void SlotSet::compact()
    {
        if (!mStale)
            return;

        std::erase_if(mSlots, [this](const Index slot) { return mFlags[slot] == 0; });
        mStale = false;
    }

    void SlotSet::clear()
    {
        for (const Index slot : mSlots)
            mFlags[slot] = 0;

        mSlots.clear();
        mStale = false;
    }
}
