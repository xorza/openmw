#include "materialtable.hpp"

#include <cassert>

namespace Rtx
{
    Index MaterialTable::add(const Material& material)
    {
        hold(material);

        const Index index = mRows.take(material);
        note(index);
        return index;
    }

    bool MaterialTable::set(Index material, const Material& what)
    {
        Material& row = mRows.at(material);
        if (row == what)
            return false;

        const bool reclassed = row.getTraversed() != what.getTraversed();

        // **The new set taken before the old is given back.** A flipbook that comes round to a frame
        // it already had names the same texture twice running; releasing first would take that slot
        // to zero, empty its path and hand it to the next thing that asked — a slot changing
        // identity under everything standing on it, on a frame where nothing was supposed to move.
        hold(what);
        drop(row);

        row = what;
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

    Run MaterialTable::addMask(std::span<const float> weights)
    {
        const Run run = mMasks.allocate(weights);
        mArrived.mMasks.push_back(run);
        return run;
    }

    Run MaterialTable::addLayers(std::span<const MaterialLayer> layers)
    {
        const Run run = mLayers.allocate(layers);
        mArrived.mLayers.push_back(run);
        return run;
    }

    std::size_t MaterialTable::mark(std::span<const Index> keep)
    {
        return mRows.mark(keep);
    }

    std::size_t MaterialTable::sweep()
    {
        return mRows.sweep([this](Index, Material& going) {
            // **What it named goes with it**, and before its layer run does: the run is what says
            // which textures those were, and it is about to be handed to an allocator that will let
            // the next chunk write over it.
            drop(going);

            // **Its layers and the masks behind them go with it.** A material that carries layers is
            // a terrain chunk, so without this what accumulates is a blend map per chunk walked
            // past; the runs are variable length, which is why they are given back to an allocator
            // rather than to a list of slots.
            for (const MaterialLayer& layer : going.mLayers.in(getLayers()))
                mMasks.release(layer.mMask);

            if (!going.mLayers.empty())
                mLayers.release(going.mLayers);

            going = Material{};
        });
    }

    void MaterialTable::clearArrivals()
    {
        mWritten.clear();
        mArrived.clear();
    }
}
