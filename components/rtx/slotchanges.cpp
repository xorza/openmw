#include "slotchanges.hpp"

#include <cassert>

namespace Rtx
{
    void SlotChanges::note(const Index slot, const SlotNews what)
    {
        assert(what != SlotNews::None);
        assert(slot < mNews.size());

        SlotNews& standing = mNews[slot];
        if (standing == what)
            return;

        if (standing == SlotNews::Arrived)
            std::erase(mArrived, slot);
        else if (standing == SlotNews::Freed)
            std::erase(mFreed, slot);

        standing = what;
        (what == SlotNews::Arrived ? mArrived : mFreed).push_back(slot);
    }

    void SlotChanges::clearArrivals()
    {
        for (const Index slot : mArrived)
            mNews[slot] = SlotNews::None;
        for (const Index slot : mFreed)
            mNews[slot] = SlotNews::None;

        mArrived.clear();
        mFreed.clear();
    }

    void SlotChanges::clear()
    {
        mNews.clear();
        mArrived.clear();
        mFreed.clear();
    }
}
