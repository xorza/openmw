#include "placementtable.hpp"

#include <cassert>

namespace Rtx
{
    Index PlacementTable::add(const MeshInstance& instance)
    {
        const Index slot = mInstances.take(instance, [this](const std::size_t slots) { mPrevious.resize(slots); });

        // Standing where it is, not arriving from wherever the last tenant left. A reused slot
        // would otherwise inherit a previous transform from something else entirely, and a motion
        // vector built from that points across the frame.
        mPrevious[slot] = instance.mTransform;

        mMoved.push_back(slot);
        ++mPlacedCount;
        return slot;
    }

    void PlacementTable::fade(const Index slot, const float opacity)
    {
        MeshInstance& placed = mInstances.at(slot);
        assert(placed.isPlaced() && "a slot nothing stands in");

        if (placed.mOpacity == opacity)
            return;

        placed.mOpacity = opacity;
        mMoved.push_back(slot);
    }

    bool PlacementTable::move(const Index slot, const osg::Matrixf& transform)
    {
        MeshInstance& placed = mInstances.at(slot);
        assert(placed.isPlaced() && "a slot nothing stands in");

        if (placed.mTransform == transform)
            return false;

        placed.mTransform = transform;
        mMoved.push_back(slot);
        return true;
    }

    void PlacementTable::drop(const Index slot)
    {
        assert(mInstances.at(slot).isPlaced() && "a slot dropped twice, or one nothing stood in");

        mInstances.at(slot) = MeshInstance{};
        mInstances.free(slot);
        mMoved.push_back(slot);
        --mPlacedCount;
    }

    void PlacementTable::advance()
    {
        for (const Index slot : mMoved)
            mPrevious[slot] = mInstances.at(slot).mTransform;

        // Swapped and not copied: the two lists trade buffers, and neither allocates on the frame.
        mSettled.swap(mMoved);
        mMoved.clear();
    }
}
