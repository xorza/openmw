#include "placementtable.hpp"

#include <cassert>

namespace Rtx
{
    Index PlacementTable::add(const MeshInstance& instance)
    {
        Index slot;
        if (mFree.empty())
        {
            slot = static_cast<Index>(mInstances.size());
            mInstances.emplace_back();
            mPrevious.emplace_back();
        }
        else
        {
            slot = mFree.back();
            mFree.pop_back();
        }

        mInstances[slot] = instance;

        // **Standing where it is, not arriving from wherever the last tenant left.** A reused slot
        // would otherwise inherit a previous transform from something else entirely, and a motion
        // vector built from that points across the frame.
        mPrevious[slot] = instance.mTransform;

        mMoved.push_back(slot);
        ++mPlacedCount;
        return slot;
    }

    void PlacementTable::fade(const Index slot, const float opacity)
    {
        assert(slot < mInstances.size());
        assert(mInstances[slot].isPlaced() && "a slot nothing stands in");

        if (mInstances[slot].mOpacity == opacity)
            return;

        mInstances[slot].mOpacity = opacity;
        mMoved.push_back(slot);
    }

    bool PlacementTable::move(const Index slot, const osg::Matrixf& transform)
    {
        assert(slot < mInstances.size());
        assert(mInstances[slot].isPlaced() && "a slot nothing stands in");

        if (mInstances[slot].mTransform == transform)
            return false;

        mInstances[slot].mTransform = transform;
        mMoved.push_back(slot);
        return true;
    }

    void PlacementTable::drop(const Index slot)
    {
        assert(slot < mInstances.size());
        assert(mInstances[slot].isPlaced() && "a slot dropped twice, or one nothing stood in");

        mInstances[slot] = MeshInstance{};
        mFree.push_back(slot);
        mMoved.push_back(slot);
        --mPlacedCount;
    }

    void PlacementTable::advance()
    {
        for (const Index slot : mMoved)
            mPrevious[slot] = mInstances[slot].mTransform;

        // Swapped and not copied: the two lists trade buffers, and neither allocates on the frame.
        mSettled.swap(mMoved);
        mMoved.clear();
    }

    void PlacementTable::clear()
    {
        mInstances.clear();
        mPrevious.clear();
        mMoved.clear();
        mSettled.clear();
        mFree.clear();
        mPlacedCount = 0;
    }
}
