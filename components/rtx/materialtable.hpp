#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "index.hpp"
#include "material.hpp"
#include "slotset.hpp"
#include "spanallocator.hpp"
#include "texturetable.hpp"

namespace Rtx
{
    /// Every material the scene holds, the terrain layers they name, and the weights those place.
    ///
    /// **One type, because a row, its runs and what they name are one invariant.** A material's
    /// slot is reference-free and a layer run is variable length, so the two are freed by different
    /// mechanisms — a free list and an allocator — and a material's textures have to be given back
    /// between them, before the run that says which they were is handed to the next chunk. That
    /// order is one call here rather than three members a caller holds in step.
    ///
    /// **The textures are borrowed and not owned.** A slot is named by materials and by holds
    /// nothing here can see, so the count lives with the table that hands slots out.
    class MaterialTable
    {
    public:
        explicit MaterialTable(TextureTable& textures)
            : mTextures(textures)
        {
        }

        std::size_t size() const { return mRows.size(); }

        /// How many slots hold a material, which is what a sweep compares its survivors against.
        std::size_t getLiveCount() const { return mRows.size() - mFree.size(); }

        Index add(const Material& material);

        /// Rewrites a material in place, keeping its slot and everything standing on it.
        ///
        /// @return whether what traversal is told about the surfaces wearing it changed — a fade
        ///         crossing opaque does, a flipbook turning does not. The caller owns what that
        ///         means to a placement, because the placements are not this table's.
        bool set(Index material, const Material& what);

        /// Copies `weights` into the shared mask table and returns where they landed.
        Index addMask(std::span<const float> weights);

        /// Copies a material's layers into the shared layer table and returns where they landed.
        Span addLayers(std::span<const MaterialLayer> layers);

        std::span<const Material> getRows() const { return mRows; }
        std::span<const MaterialLayer> getLayers() const { return mLayers; }
        std::span<const float> getMasks() const { return mMasks; }

        std::span<const Index> getWritten() const { return mWritten.getSlots(); }
        std::span<const Span> getArrivedLayers() const { return mArrivedLayers; }
        std::span<const Span> getArrivedMasks() const { return mArrivedMasks; }

        /// Notes every slot a sweep must not free, and says how many distinct ones `keep` named.
        ///
        /// **Apart from `sweep`, because a caller sweeps two tables or neither.** A scene marks its
        /// meshes and its materials, and frees nothing where both came back whole.
        std::size_t mark(std::span<const Index> keep);

        /// Frees every slot the last `mark` did not name, and says how many that was.
        std::size_t sweep();

        void clearArrivals();

    private:
        /// Records that `slot`'s row was written, once however many times it is.
        void note(Index slot);

        /// Every texture slot `material` names — its three roles, and every layer of its run.
        ///
        /// **One walk, because taking and giving back are the same four steps with one call
        /// swapped.** A role added to one of that pair and forgotten in the other frees a slot
        /// something still stands on, or holds one nothing gives back.
        ///
        /// The layer run is already in the layer table: a caller builds its layers, places them
        /// with `addLayers` and then hands over a material naming where they landed.
        template <class Visit>
        void forEachTexture(const Material& material, Visit visit) const
        {
            visit(material.mDiffuse);
            visit(material.mNormal);
            visit(material.mEmissive);

            for (Index at = 0; at < material.mLayerCount; ++at)
                visit(mLayers[material.mLayerOffset + at].mDiffuse);
        }

        /// Takes and gives back those slots. Only ever called in that pair, and `set` is why the
        /// order between them matters.
        void hold(const Material& material);
        void drop(const Material& material);

        TextureTable& mTextures;

        std::vector<Material> mRows;
        std::vector<MaterialLayer> mLayers;
        std::vector<float> mMasks;

        /// Slots nothing stands in, as a min-heap. `Rtx::takeFreeSlot` says why the lowest.
        std::vector<Index> mFree;

        /// Which slots a sweep was told to keep, one flag per row.
        ///
        /// **Held rather than made, because a sweep runs on the frame a cell left** — the frame
        /// that is already giving thousands of runs back to the allocators, and the last one that
        /// should also be sizing a buffer to the whole table.
        std::vector<std::uint8_t> mKept;

        /// Rows written since the last `clearArrivals` — a flipbook that is added and then
        /// rewritten on one frame is one row, not two.
        SlotSet mWritten;

        /// Runs placed since the last `clearArrivals`.
        std::vector<Span> mArrivedLayers;
        std::vector<Span> mArrivedMasks;

        /// Where a material's layers and a layer's weights live.
        ///
        /// **Runs and not slots**, which is why these are allocators and `mFree` is not: a material
        /// is one material's worth of room, but a terrain chunk's layer run is as long as the
        /// ground types under it and its masks are as big as the blend maps. A list of slots cannot
        /// give a variable length back.
        SpanAllocator mLayerRuns;
        SpanAllocator mMaskRuns;
    };
}
