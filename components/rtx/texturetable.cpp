#include "texturetable.hpp"

#include <cassert>

namespace Rtx
{
    Index TextureTable::takeSlot()
    {
        ++mRevision;

        // One size, so any freed slot will do — the array element it names is written over wherever
        // it sits, which is what the arrivals list is for. The two name tables and the change list
        // follow the rows in the same call, because all four are indexed by the slot.
        const Index index = mSlots.take(Slot{}, [this](const std::size_t slots) {
            mPaths.resize(slots);
            mBaked.resize(slots);
            mChanges.grow(slots);
        });

        assert(mSlots.at(index).mRefs == 0 && "a free slot something still names");

        return index;
    }

    Index TextureTable::add(const VFS::Path::NormalizedView path)
    {
        const auto known = mPathIndex.find(path);
        if (known != mPathIndex.end())
            return known->second;

        const Index index = takeSlot();
        mPaths[index] = path;
        mSlots.at(index).mKind = Kind::File;

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
        mSlots.at(index).mKind = Kind::Baked;

        mBakedIndex.emplace(key, index);
        mChanges.note(index, SlotNews::Arrived);
        return index;
    }

    void TextureTable::hold(const Index texture)
    {
        if (texture == sNoIndex)
            return;

        ++mSlots.at(texture).mRefs;
    }

    void TextureTable::drop(const Index texture)
    {
        if (texture == sNoIndex)
            return;

        Slot& slot = mSlots.at(texture);
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
        mSlots.free(texture);
        mChanges.note(texture, SlotNews::Freed);
    }
}
