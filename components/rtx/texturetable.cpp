#include "texturetable.hpp"

#include <cassert>

#include "slotrows.hpp"

namespace Rtx
{
    Index TextureTable::takeSlot()
    {
        ++mRevision;

        // One size, so any freed slot will do — the array element it names is written over wherever
        // it sits, which is what the arrivals list is for.
        if (mFree.empty())
        {
            mPaths.emplace_back();
            mBaked.emplace_back();
            mSlots.emplace_back();
            mChanges.grow(mSlots.size());
            return static_cast<Index>(mSlots.size() - 1);
        }

        const Index index = takeFreeSlot(mFree);
        assert(mSlots[index].mRefs == 0 && "a free slot something still names");

        return index;
    }

    Index TextureTable::add(const VFS::Path::NormalizedView path)
    {
        const auto known = mPathIndex.find(path);
        if (known != mPathIndex.end())
            return known->second;

        const Index index = takeSlot();
        mPaths[index] = path;
        mSlots[index].mKind = Kind::File;

        mPathIndex.emplace(path, index);
        mChanges.note(index, SlotNews::Arrived);
        return index;
    }

    Index TextureTable::addBaked(const std::string_view key)
    {
        assert(!key.empty() && "a baked texture with no key is one nothing can find again");

        const auto known = mBakedIndex.find(key);
        if (known != mBakedIndex.end())
            return known->second;

        const Index index = takeSlot();
        mBaked[index] = key;
        mSlots[index].mKind = Kind::Baked;

        mBakedIndex.emplace(key, index);
        mChanges.note(index, SlotNews::Arrived);
        return index;
    }

    void TextureTable::hold(const Index texture)
    {
        if (texture == sNoIndex)
            return;

        assert(texture < mSlots.size());
        ++mSlots[texture].mRefs;
    }

    void TextureTable::drop(const Index texture)
    {
        if (texture == sNoIndex)
            return;

        assert(texture < mSlots.size());

        Slot& slot = mSlots[texture];
        assert(slot.mRefs > 0 && "a texture given back more often than it was taken");

        if (--slot.mRefs > 0)
            return;

        // The name leaves the lookup with the slot, or the next reference to it resolves to a slot
        // nothing is standing in.
        switch (slot.mKind)
        {
            case Kind::File:
                mPathIndex.erase(mPaths[texture]);
                mPaths[texture] = VFS::Path::Normalized();
                break;
            case Kind::Baked:
                mBakedIndex.erase(mBaked[texture]);
                mBaked[texture].clear();
                break;
            case Kind::Free:
                assert(false && "a slot with a reference to give back that nothing ever named");
                break;
        }

        slot.mKind = Kind::Free;
        freeSlot(mFree, texture);
        mChanges.note(texture, SlotNews::Freed);
    }

    void TextureTable::clear()
    {
        mPaths.clear();
        mBaked.clear();
        mSlots.clear();
        mFree.clear();
        mChanges.clearArrivals();
        mPathIndex.clear();
        mBakedIndex.clear();
    }
}
