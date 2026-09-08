#include "slotchanges.hpp"

namespace Rtx
{
    void SlotChanges::note(const Index slot, const SlotNews what)
    {
        const bool arriving = what == SlotNews::Arrived;
        SlotSet& taking = arriving ? mArrived : mFreed;
        SlotSet& giving = arriving ? mFreed : mArrived;

        // **Compacted here rather than left for the reader**, because the other set is asked for
        // straight after this returns and a slot changes its news rarely enough to pay a pass then.
        giving.remove(slot);
        giving.compact();

        taking.add(slot);
    }
}
