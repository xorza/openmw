#include "materialtable.hpp"

#include <algorithm>
#include <cassert>

#include "slotrows.hpp"

namespace Rtx
{
    Index MaterialTable::add(const Material& material)
    {
        hold(material);

        const Index index = takeSlot(mRows, mFree, material);
        note(index);
        return index;
    }

    bool MaterialTable::set(Index material, const Material& what)
    {
        assert(material < mRows.size());

        if (mRows[material] == what)
            return false;

        const bool reclassed = mRows[material].getTraversed() != what.getTraversed();

        // **The new set taken before the old is given back.** A flipbook that comes round to a frame
        // it already had names the same texture twice running; releasing first would take that slot
        // to zero, empty its path and hand it to the next thing that asked — a slot changing
        // identity under everything standing on it, on a frame where nothing was supposed to move.
        hold(what);
        drop(mRows[material]);

        mRows[material] = what;
        note(material);

        return reclassed;
    }

    void MaterialTable::note(Index slot)
    {
        mWritten.grow(mRows.size());
        mWritten.add(slot);
    }

    void MaterialTable::hold(const Material& material)
    {
        forEachTexture(material, [this](const Index texture) { mTextures.hold(texture); });
    }

    void MaterialTable::drop(const Material& material)
    {
        forEachTexture(material, [this](const Index texture) { mTextures.drop(texture); });
    }

    Index MaterialTable::addMask(std::span<const float> weights)
    {
        assert(!weights.empty());

        const Span run = mMaskRuns.allocate(static_cast<std::uint32_t>(weights.size()));

        // Grown and never shrunk: a hole at the end gives its room back to the allocator, and the
        // next chunk to arrive lands in it rather than in a table that had to be resized twice.
        if (mMasks.size() < mMaskRuns.getEnd())
            mMasks.resize(mMaskRuns.getEnd());

        std::copy(weights.begin(), weights.end(), mMasks.begin() + run.mOffset);
        mArrivedMasks.push_back(run);
        return run.mOffset;
    }

    Span MaterialTable::addLayers(std::span<const MaterialLayer> layers)
    {
        assert(!layers.empty());

        const Span run = mLayerRuns.allocate(static_cast<std::uint32_t>(layers.size()));

        if (mLayers.size() < mLayerRuns.getEnd())
            mLayers.resize(mLayerRuns.getEnd());

        std::copy(layers.begin(), layers.end(), mLayers.begin() + run.mOffset);
        mArrivedLayers.push_back(run);
        return run;
    }

    std::size_t MaterialTable::mark(std::span<const Index> keep)
    {
        return markKept(mKept, mRows.size(), keep, mFree);
    }

    std::size_t MaterialTable::sweep()
    {
        std::size_t freed = 0;
        for (Index index = 0; index < mRows.size(); ++index)
        {
            if (mKept[index] != 0)
                continue;

            // **What it named goes with it**, and before its layer run does: the run is what says
            // which textures those were, and it is about to be handed to an allocator that will let
            // the next chunk write over it.
            const Material& going = mRows[index];
            drop(going);

            // **Its layers and the masks behind them go with it.** A material that carries layers is
            // a terrain chunk, so without this what accumulates is a blend map per chunk walked
            // past; the runs are variable length, which is why they are given back to an allocator
            // rather than to a list of slots.
            for (Index at = 0; at < going.mLayerCount; ++at)
            {
                const MaterialLayer& layer = mLayers[going.mLayerOffset + at];
                mMaskRuns.release(Span{ .mOffset = layer.mMaskOffset,
                    .mCount = static_cast<std::uint32_t>(layer.mMaskWidth) * layer.mMaskHeight });
            }

            if (going.mLayerCount > 0)
                mLayerRuns.release(Span{ .mOffset = going.mLayerOffset, .mCount = going.mLayerCount });

            mRows[index] = Material{};
            freeSlot(mFree, index);
            ++freed;
        }

        return freed;
    }

    void MaterialTable::clearArrivals()
    {
        mWritten.clear();
        mArrivedLayers.clear();
        mArrivedMasks.clear();
    }
}
