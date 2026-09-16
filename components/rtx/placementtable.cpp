#include "placementtable.hpp"

#include <cassert>
#include <cstddef>

namespace Rtx
{
    Index PlacementTable::add(const MeshInstance& instance)
    {
        const Index slot = mInstances.take(instance, [this](const std::size_t slots) {
            mPrevious.resize(slots);
            mNextWearing.resize(slots, sNoIndex);
            mPrevWearing.resize(slots, sNoIndex);
        });

        // Standing where it is, not arriving from wherever the last tenant left. A reused slot
        // would otherwise inherit a previous transform from something else entirely, and a motion
        // vector built from that points across the frame.
        mPrevious[slot] = instance.mTransform;

        link(slot, instance.mMaterial);

        mMoved.push_back(slot);
        ++mPlacedCount;
        return slot;
    }

    void PlacementTable::link(const Index slot, const Index material)
    {
        if (material == sNoIndex)
            return;

        // Grown with the materials rather than with the slots, on the arrival that first names a
        // material this many: the table of materials grows on the same frame.
        if (material >= mFirstWearing.size())
            mFirstWearing.resize(std::size_t{ material } + 1, sNoIndex);

        const Index first = mFirstWearing[material];
        mNextWearing[slot] = first;
        mPrevWearing[slot] = sNoIndex;
        if (first != sNoIndex)
            mPrevWearing[first] = slot;
        mFirstWearing[material] = slot;
    }

    void PlacementTable::unlink(const Index slot, const Index material)
    {
        if (material == sNoIndex)
            return;

        const Index next = mNextWearing[slot];
        const Index previous = mPrevWearing[slot];
        if (previous != sNoIndex)
            mNextWearing[previous] = next;
        else
            mFirstWearing[material] = next;
        if (next != sNoIndex)
            mPrevWearing[next] = previous;

        mNextWearing[slot] = sNoIndex;
        mPrevWearing[slot] = sNoIndex;
    }

    void PlacementTable::rewriteWearing(const Index material)
    {
        if (material >= mFirstWearing.size())
            return;

        for (Index slot = mFirstWearing[material]; slot != sNoIndex; slot = mNextWearing[slot])
            mMoved.push_back(slot);
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

        unlink(slot, mInstances.at(slot).mMaterial);
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
