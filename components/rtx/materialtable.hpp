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
    /// The runs a material table has placed since the last `clearArrivals`.
    ///
    /// **Two lists and not a `SlotSet`**, because a run is an offset and a count rather than a slot
    /// and the set cannot hold one. Named together because they are filled together, cleared
    /// together and read together by whatever uploads them.
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

    class MaterialTable : public SweptTable<Material>
    {
    public:
        explicit MaterialTable(TextureTable& textures)
            : mTextures(textures)
        {
        }

        Index add(const Material& material);

        /// Rewrites a material in place, keeping its slot and everything standing on it.
        ///
        /// @return whether what traversal is told about the surfaces wearing it changed — a fade
        ///         crossing opaque does, a flipbook turning does not. The caller owns what that
        ///         means to a placement, because the placements are not this table's.
        bool set(Index material, const Material& what);

        /// Copies `weights` into the shared mask table and returns where they landed.
        ///
        /// One float per weight rather than the byte the source holds: a mask is a few hundred
        /// texels and a whole cell's worth is tens of kilobytes, which is not worth requiring
        /// 8-bit storage of the device for.
        ///
        /// The whole run comes back, so a layer gives back exactly what it took rather than what
        /// its two sides multiply to.
        Run addMask(std::span<const float> weights);

        /// Copies a material's layers into the shared layer table and returns where they landed.
        ///
        /// **All of them at once, because a run is allocated as a run.** They were appended one at
        /// a time when the table only ever grew and a material took whatever length the table
        /// happened to be at; a run that can be given back has to be asked for by length.
        Run addLayers(std::span<const MaterialLayer> layers);

        std::span<const MaterialLayer> getLayers() const { return mLayers.getAll(); }
        std::span<const float> getMasks() const { return mMasks.getAll(); }

        std::span<const Index> getWritten() const { return mWritten.getSlots(); }
        const ArrivedRuns& getArrived() const { return mArrived; }

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

            for (const MaterialLayer& layer : material.mLayers.in(getLayers()))
                visit(layer.mDiffuse);
        }

        /// Takes and gives back those slots. Only ever called in that pair, and `set` is why the
        /// order between them matters.
        void holdTextures(const Material& material);
        void dropTextures(const Material& material);

        TextureTable& mTextures;

        /// Rows written since the last `clearArrivals` — a flipbook that is added and then
        /// rewritten on one frame is one row, not two.
        SlotSet mWritten;

        ArrivedRuns mArrived;

        /// A material's layers, and the weights a layer places.
        ///
        /// **Runs and not slots**, which is why these are `RunBuffer`s and `mRows` is not: a
        /// material is one material's worth of room, but a terrain chunk's layer run is as long as
        /// the ground types under it and its masks are as big as the blend maps. A list of slots
        /// cannot give a variable length back.
        RunBuffer<MaterialLayer> mLayers;
        RunBuffer<float> mMasks;
    };
}
