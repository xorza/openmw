#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "material.hpp"
#include "runs.hpp"
#include "slots.hpp"
#include "texturetable.hpp"

namespace Rtx
{
    /// The runs a material table has placed since the last `clearArrivals`. Two lists and not a
    /// `SlotSet`, because a run is an offset and a count rather than a slot.
    struct ArrivedRuns
    {
        std::vector<Run> mLayers;
        std::vector<Run> mMasks;

        void clear()
        {
            mLayers.clear();
            mMasks.clear();
        }
    };

    /// Every material the scene holds, the terrain layers they name, and the weights those place.
    /// One type, because a material's textures have to be given back before the run that says
    /// which they were is handed to the next chunk. The textures are borrowed and not owned: a slot
    /// is named by holds nothing here can see.
    class MaterialTable
    {
    public:
        /// The half of `SlotRows` a reader and a sweep use. A row goes in through `add` and out
        /// through `sweep`, so `take`, `at` and the sweep stay this table's own.
        std::size_t size() const { return mRows.size(); }
        std::size_t getLiveCount() const { return mRows.getLiveCount(); }
        bool isLive(Index slot) const { return mRows.isLive(slot); }
        std::span<const Material> getRows() const { return mRows.getRows(); }
        void hold(Index slot) { mRows.hold(slot); }
        bool drop(Index slot) { return mRows.drop(slot); }
        bool hasDroppedHolds() const { return mRows.hasDroppedHolds(); }
        std::size_t mark(std::span<const Index> keep) { return mRows.mark(keep); }

        explicit MaterialTable(TextureTable& textures)
            : mTextures(textures)
        {
        }

        Index add(const Material& material);

        /// Rewrites a material in place, keeping its slot and everything standing on it, and says
        /// whether what traversal is told about the surfaces wearing it changed — a fade crossing
        /// opaque does, a flipbook turning does not.
        bool set(Index material, const Material& what);

        /// Copies `weights` into the shared mask table and returns where they landed. One float per
        /// weight rather than the byte the source holds: a cell's worth is tens of kilobytes.
        Run addMask(std::span<const float> weights);

        /// Copies a material's layers into the shared layer table and returns where they landed —
        /// all of them at once, because a run that can be given back has to be asked for by length.
        Run addLayers(std::span<const MaterialLayer> layers);

        std::span<const MaterialLayer> getLayers() const { return mLayers.getAll(); }
        std::span<const float> getMasks() const { return mMasks.getAll(); }

        std::span<const Index> getWritten() const { return mWritten.getSlots(); }
        const ArrivedRuns& getArrived() const { return mArrived; }

        /// How many runs have been placed, ever. What a backend checks it staged the arrivals
        /// against: a run is written on arrival and never again, so one that arrived where the
        /// backend was not told would be read stale for its life.
        std::uint64_t getRunRevision() const { return mRunRevision; }

        /// Frees every slot the last `mark` did not name, and says how many that was.
        std::size_t sweep();

        void clearArrivals();

    private:
        /// Records that `slot`'s row was written, once however many times it is.
        void note(Index slot);

        /// Every texture slot `material` names — its two roles, and every layer of its run. One
        /// walk, or a role added to the hold and forgotten in the drop frees a slot something still
        /// stands on.
        template <class Visit>
        void forEachTexture(const Material& material, Visit visit) const
        {
            visit(material.mDiffuse);
            visit(material.mEmissive);

            for (const MaterialLayer& layer : material.mLayers.in(getLayers()))
                visit(layer.mDiffuse);
        }

        /// Takes and gives back those slots. Only ever called in that pair, and `set` is why the
        /// order between them matters.
        void holdTextures(const Material& material);
        void dropTextures(const Material& material);

        TextureTable& mTextures;

        SlotRows<Material> mRows;

        /// Rows written since the last `clearArrivals` — a flipbook that is added and then
        /// rewritten on one frame is one row, not two.
        SlotSet mWritten;

        ArrivedRuns mArrived;
        std::uint64_t mRunRevision = 0;

        /// A material's layers, and the weights a layer places — runs and not slots, because a
        /// terrain chunk's layer run is as long as the ground types under it.
        RunBuffer<MaterialLayer> mLayers;
        RunBuffer<float> mMasks;
    };
}
