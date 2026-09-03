#include "texturetable.hpp"

#include <cassert>

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
            mRefs.push_back(0);
            mChanges.grow(mPaths.size());
            return static_cast<Index>(mPaths.size() - 1);
        }

        const Index index = mFree.back();
        mFree.pop_back();
        assert(mRefs[index] == 0 && "a free slot something still names");

        return index;
    }

    Index TextureTable::add(const VFS::Path::NormalizedView path)
    {
        const auto known = mPathIndex.find(path);
        if (known != mPathIndex.end())
            return known->second;

        const Index index = takeSlot();
        mPaths[index] = path;

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

        mBakedIndex.emplace(key, index);
        mChanges.note(index, SlotNews::Arrived);
        return index;
    }

    void TextureTable::hold(const Index texture)
    {
        if (texture == sNoIndex)
            return;

        assert(texture < mRefs.size());
        ++mRefs[texture];
    }

    void TextureTable::drop(const Index texture)
    {
        if (texture == sNoIndex)
            return;

        assert(texture < mRefs.size());
        assert(mRefs[texture] > 0 && "a texture given back more often than it was taken");

        if (--mRefs[texture] > 0)
            return;

        // The name leaves the lookup with the slot, or the next reference to it resolves to a slot
        // nothing is standing in. Whichever of the two named it, and never both: a slot is a file or
        // it is something this renderer made.
        if (!mPaths[texture].empty())
        {
            mPathIndex.erase(mPaths[texture]);
            mPaths[texture] = VFS::Path::Normalized();
        }
        else
        {
            assert(!mBaked[texture].empty() && "a slot with a reference to give back that nothing ever named");
            mBakedIndex.erase(mBaked[texture]);
            mBaked[texture].clear();
        }

        mFree.push_back(texture);
        mChanges.note(texture, SlotNews::Freed);
    }

    void TextureTable::clear()
    {
        mPaths.clear();
        mBaked.clear();
        mRefs.clear();
        mFree.clear();
        mChanges.clear();
        mPathIndex.clear();
        mBakedIndex.clear();
    }
}
