#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <osg/Vec3f>
#include <osg/Vec4f>

#include <components/misc/constants.hpp>

#include "texturedata.hpp"

namespace Rtx
{
    /// How wide a chunk has to be before its stack is flattened rather than shaded live.
    ///
    /// **One cell, which is the same answer the rasterizer reaches** — `chunkSize >= 1` is what
    /// `ChunkManager` composites at, and a quad tree only builds a chunk that wide once distance has
    /// already cost it its geometric detail. Shading detail going with it is consistent rather than
    /// arbitrary.
    inline constexpr float sCompositeFrom = static_cast<float>(Constants::CellSizeInUnits);

    /// How large a baked composite is, square.
    ///
    /// The rasterizer's `composite map resolution` in all but name, and its default; stated here
    /// rather than read from it because this path forces that setting past every chunk and would be
    /// taking a number from a knob it has just declared meaningless.
    inline constexpr std::uint32_t sCompositeExtent = 512;

    /// How much painted light a bake divides out.
    ///
    /// **Full, because that is what every frame asks for.** The strength is a frame constant the
    /// shader reads, and a composite cannot be corrected later — the estimate repeats with a
    /// texture's tiling and a composite has none. `--delight` therefore reaches the near field and
    /// not distant ground, which is a diagnostic knob telling half a story rather than a wrong
    /// picture.
    inline constexpr float sCompositeDelight = 1.0f;

    /// One layer of the stack a chunk's ground is drawn from, as a bake needs it.
    ///
    /// The same four facts `MaterialLayer` carries, with the images themselves in place of the slots
    /// they were put in: a bake reads texels, and the scene's table holds indices.
    struct CompositeLayer
    {
        /// The tiling ground texture, decoded, with whatever mip chain its file carried.
        TextureData mDiffuse;

        /// The light already painted into that texture, `ShadingMap::sExtent` squared factors.
        /// Empty is neutral, which is what a texture nothing could estimate one for gets.
        std::span<const float> mShading;

        /// Chunk texture coordinates to this layer's, as `uv * xy + zw` — the shader's spelling, so
        /// the transforms the extractor read off the terrain builder come across unchanged.
        osg::Vec4f mDiffuseTransform{ 1.0f, 1.0f, 0.0f, 0.0f };

        /// The weights this layer shows through, row by row. Empty covers the chunk entirely, which
        /// is what a chunk of a single ground type gets.
        std::span<const float> mMask;
        std::uint32_t mMaskWidth = 0;
        std::uint32_t mMaskHeight = 0;
        osg::Vec4f mMaskTransform{ 1.0f, 1.0f, 0.0f, 0.0f };
    };

    /// One level of a layer's diffuse, decoded to linear once.
    ///
    /// **DecodedLevel up front rather than a block per tap.** The level a bake reads is the one whose
    /// texels are the size of one composite texel, so for ground tiling sixty times across a chunk
    /// it is a handful of texels square — while the composite takes a quarter of a million samples
    /// from it. Reading a compressed block at every tap made a chunk cost 56 ms; reading each level
    /// once makes every tap an array lookup and changes not one texel of the answer.
    struct DecodedLevel
    {
        std::vector<osg::Vec3f> mTexels;
        std::uint32_t mWidth = 0;
        std::uint32_t mHeight = 0;

        bool isEmpty() const { return mTexels.empty(); }

        /// Empties it without giving its room back, so the next bake refills what the last one grew.
        void reuse()
        {
            mTexels.clear();
            mWidth = 0;
            mHeight = 0;
        }
    };

    /// One layer's diffuse, reduced to the two levels the whole bake will read and how far it sits
    /// between them.
    struct Ground
    {
        DecodedLevel mFine;
        DecodedLevel mCoarse;
        float mBetween = 0.0f;

        void reuse()
        {
            mFine.reuse();
            mCoarse.reuse();
            mBetween = 0.0f;
        }
    };

    /// Everything a bake writes that is not its answer, held by whoever bakes rather than made per
    /// chunk.
    ///
    /// **A bake's working set is larger than what it produces.** The sum alone is one `osg::Vec3f` a
    /// texel — three megabytes at `sCompositeExtent` — and a nine-layer stack decodes eighteen
    /// levels beside it. A crossing queues dozens of chunks, so making that per chunk is the same
    /// megabytes taken and given back dozens of times over a load.
    ///
    /// **One thread's, and it is the caller that says which.** `CompositeQueue` holds one on its
    /// baker and hands it to every bake; nothing here is guarded, because nothing else may touch it.
    struct CompositeScratch
    {
        /// One per layer of the deepest stack met so far, each keeping the levels it decoded. Never
        /// shrunk: a shorter stack uses the front of it and leaves the rest holding their room.
        std::vector<Ground> mGrounds;

        /// The sum, in light, one entry a texel of the level being built.
        std::vector<osg::Vec3f> mLight;

        /// The level under it, while the chain is being reduced. **Swapped with `mLight` at every
        /// level**, so the two take turns holding the finer half and both are reserved to the
        /// finest.
        std::vector<osg::Vec3f> mCoarser;

        /// Which mask columns hold anything on the row being summed. See `coveredColumns`.
        std::vector<std::uint8_t> mCovered;
    };

    /// A chunk's whole layer stack, flattened into one texture.
    ///
    /// **The composite is the shading LOD and not only a way around a render target.** A distant
    /// chunk covers many cells and carries every ground type in them; shading it live is a mask
    /// lookup and a texture fetch per layer per hit, and distant hits are most of the pixels once
    /// there is distance to look at. This turns that into one fetch, and the near field keeps the
    /// live stack where the layer count is small and the sharpness is worth paying for.
    ///
    /// **On the CPU and in the core**, so it is written once for both backends, needs no device to
    /// test, and reaches the uploader as the same `TextureData` a file does. The GL renderer answers
    /// the same question with `Terrain::CompositeMapRenderer`, which this path has no context for.
    ///
    /// **Everything is summed in light.** Each layer's texel is decoded, has its painted light
    /// divided out, is weighted by its mask and only then re-encoded — the same order the shader
    /// reaches at a hit, and the reason a half-and-half blend comes out at 188 rather than 128.
    ///
    /// **Whole, and never on the frame.** A chunk costs **27 ms** to flatten — measured over three
    /// runs of the island route at 280-odd chunks each, nine ground types apiece against a mask 34
    /// across — and that is a dropped frame however good the average is. Sliced sixteen rows a
    /// frame it was a millisecond or two on every frame for twenty seconds after a load instead,
    /// which is the other way of being on the frame. So `CompositeQueue` builds one on a thread of
    /// its own and hands the frame the bytes; nothing here is shaped for stopping part way, and the
    /// spans a caller passes are read inside the constructor and never again.
    ///
    /// Two changes made it that, each a measured halving: decoding every level once instead of a
    /// compressed block at every tap, and then walking the stack a layer and a row at a time rather
    /// than a texel at a time, which took it from 53 ms. What is left is a quarter of a million
    /// output texels, each summing the ground types whose masks reach it.
    class TerrainComposite
    {
    public:
        /// Flattens the stack at `extent` square: every texel summed and the chain built.
        ///
        /// @param extent a power of two, so the chain halves exactly and ends at one texel.
        /// @param delight how much of each layer's painted light to divide out, matching the frame
        ///        constant the shader reads. **Baked in rather than left to the shader**, because
        ///        the estimate repeats with the texture's tiling and the composite has none: this is
        ///        the last point at which the tiling is still known.
        /// @param scratch what the bake works in, which is the caller's so that a queue of them
        ///        pays for it once. Cleared and refilled here, and read by nothing afterwards.
        TerrainComposite(
            std::span<const CompositeLayer> layers, std::uint32_t extent, float delight, CompositeScratch& scratch);

        /// Moved and never copied, like every other description that hands out spans of itself: two
        /// composites holding the same texels under one key is two answers to a question with one.
        TerrainComposite(const TerrainComposite&) = delete;
        TerrainComposite& operator=(const TerrainComposite&) = delete;
        TerrainComposite(TerrainComposite&&) noexcept = default;
        TerrainComposite& operator=(TerrainComposite&&) noexcept = default;

        /// The baked image, spanning storage this object owns and carrying a neutral shading map.
        ///
        /// `mSlot` and `mName` are the caller's to fill: the scene decides where a composite goes
        /// and what key found it.
        TextureData describe() const;

        std::uint32_t getLevelCount() const { return static_cast<std::uint32_t>(mLevels.size()); }

    private:
        /// Reduces the summed light to the chain of encoded bytes a backend uploads, spending it.
        void buildChain(CompositeScratch& scratch);

        std::vector<std::byte> mBytes;
        std::vector<MipLevel> mLevels;
        std::uint32_t mExtent = 0;
    };
}
