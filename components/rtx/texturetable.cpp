#include "texturetable.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <string>
#include <utility>

#include "contract.hpp"

namespace Rtx
{
    bool TextureTable::hasRoom()
    {
        if (mRows.getLiveCount() < sCapacity)
            return true;

        // Counted and not reported: a table does not reach the scene's `Refusals`, and
        // `SceneTextures` reports the limit with the rest of what an arrival stood in for.
        ++mRefused;
        return false;
    }

    Index TextureTable::takeSlot(TextureRow row)
    {
        ++mRevision;

        // One size, so any freed slot will do — the row is written over wherever it sits, which
        // is what the arrivals list is for. The change list follows the rows in the same call,
        // because it is indexed by the slot.
        const Index index = mRows.take(std::move(row));
        mChanges.grow(mRows.size());
        mChanges.note(index, SlotNews::Arrived);

        return index;
    }

    Index TextureTable::add(
        const VFS::Path::NormalizedView path, const TextureWrap wrap, const TextureEncoding encoding)
    {
        const auto as = static_cast<std::size_t>(encoding);
        const auto at = static_cast<std::size_t>(wrap);

        auto known = mPathIndex.find(path);
        if (known != mPathIndex.end() && known->second[as][at] != sNoIndex)
            return known->second[as][at];

        if (!hasRoom())
            return sNoIndex;

        const Index index = takeSlot(TextureRow{
            .mKind = TextureKind::File,
            .mPath = VFS::Path::Normalized(path),
            .mWrap = wrap,
            .mEncoding = encoding,
        });

        if (known == mPathIndex.end())
        {
            FileSlots none;
            for (auto& slots : none)
                slots.fill(sNoIndex);
            known = mPathIndex.emplace(path, none).first;
        }
        known->second[as][at] = index;

        return index;
    }

    Index TextureTable::findFile(const VFS::Path::NormalizedView path) const
    {
        const auto known = mPathIndex.find(path);
        if (known == mPathIndex.end())
            return sNoIndex;

        for (const Index slot : known->second[static_cast<std::size_t>(TextureEncoding::Colour)])
            if (slot != sNoIndex)
                return slot;

        return sNoIndex;
    }

    Index TextureTable::addBaked(const std::string_view key)
    {
        assert(!key.empty() && "a baked texture with no key is one nothing can find again");

        const auto known = mBakedIndex.find(key);
        if (known != mBakedIndex.end())
            return known->second;

        if (!hasRoom())
            return sNoIndex;

        const Index index = takeSlot(TextureRow{
            .mKind = TextureKind::Baked,
            .mBaked = std::string(key),
            .mWrap = TextureWrap::Clamp,
        });

        mBakedIndex.emplace(key, index);
        return index;
    }

    void TextureTable::hold(const Index texture)
    {
        if (texture >= sCapacity)
            return;

        mRows.hold(texture);
    }

    void TextureTable::drop(const Index texture)
    {
        if (texture >= sCapacity)
            return;

        if (!mRows.drop(texture))
            return;

        TextureRow& row = mRows.at(texture);

        // The name leaves the lookup with the slot, or the next reference to it resolves to a slot
        // nothing is standing in.
        switch (row.mKind)
        {
            case TextureKind::File:
            {
                const auto known = mPathIndex.find(row.mPath);
                contract(known != mPathIndex.end(), "a file slot the path index does not know");
                FileSlots& held = known->second;
                held[static_cast<std::size_t>(row.mEncoding)][static_cast<std::size_t>(row.mWrap)] = sNoIndex;
                if (std::ranges::all_of(held, [](const auto& slots) {
                        return std::ranges::all_of(slots, [](const Index slot) { return slot == sNoIndex; });
                    }))
                    mPathIndex.erase(known);
                break;
            }
            case TextureKind::Baked:
                mBakedIndex.erase(row.mBaked);
                break;
            case TextureKind::Free:
                assert(false && "a slot with a reference to give back that nothing ever named");
                break;
        }

        row = TextureRow{};
        mRows.free(texture);
        mChanges.note(texture, SlotNews::Freed);
    }
}
