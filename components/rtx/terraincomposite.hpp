#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <osg/Vec3f>
#include <osg/Vec4f>

#include "ownedtexture.hpp"
#include "texturedata.hpp"

namespace Rtx
{
    /// How large a baked composite is, square: the rasterizer's `composite map resolution`
    /// default, stated here because this path forces that setting past every chunk.
    inline constexpr std::uint32_t sCompositeExtent = 512;

    /// How much painted light a bake divides out: full, because a composite cannot be corrected
    /// later — the estimate repeats with a texture's tiling and a composite has none — so
    /// `--delight` reaches the near field and not distant ground.
    inline constexpr float sCompositeDelight = 1.0f;

    /// One layer of the stack a chunk's ground is drawn from, as a bake needs it: the same four
    /// facts `MaterialLayer` carries, with the images themselves in place of the slots they were
    /// put in, because a bake reads texels and the scene's table holds indices.
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

    /// One level of a layer's diffuse, decoded to linear once: the level a bake reads is a handful
    /// of texels square, and the composite takes a quarter of a million samples from it.
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
    /// chunk: the sum alone is three megabytes at `sCompositeExtent`, and a crossing queues dozens
    /// of chunks. One thread's, unguarded, because `CompositeQueue` holds one on each baker.
    struct CompositeScratch
    {
        /// One per layer of the deepest stack met so far, each keeping the levels it decoded. Never
        /// shrunk: a shorter stack uses the front of it and leaves the rest holding their room.
        std::vector<Ground> mGrounds;

        /// The sum, in light, one entry a texel of the level being built.
        std::vector<osg::Vec3f> mLight;

        /// The level under it, while the chain is being reduced. Swapped with `mLight` at every
        /// level, so the two take turns holding the finer half and both are reserved to the
        /// finest.
        std::vector<osg::Vec3f> mCoarser;

        /// Which mask columns hold anything on the row being summed. See `coveredColumns`.
        std::vector<std::uint8_t> mCovered;
    };

    /// A chunk's whole layer stack, flattened into one texture — the shading LOD: a distant chunk
    /// carries every ground type in many cells, and distant hits are most of the pixels. On the
    /// CPU and in the core, so it reaches the uploader as the same `TextureData` a file does; the
    /// GL renderer's `Terrain::CompositeMapRenderer` needs a context this path has not got.
    /// Everything is summed in light — decoded, delighted, weighted, then re-encoded, the order the
    /// shader reaches at a hit, and why a half-and-half blend comes out at 188 rather than 128.
    /// Whole and never on the frame: a chunk takes tens of milliseconds, so `CompositeQueue` builds
    /// one on a thread of its own, and the spans a caller passes are read inside the constructor
    /// and never again.
    class TerrainComposite
    {
    public:
        /// Flattens the stack at `extent` square: every texel summed and the chain built.
        ///
        /// @param extent a power of two, so the chain halves exactly and ends at one texel.
        /// @param delight how much of each layer's painted light to divide out, baked in because
        ///        this is the last point at which the texture's tiling is still known.
        /// @param scratch what the bake works in, the caller's so that a queue of them pays once.
        TerrainComposite(
            std::span<const CompositeLayer> layers, std::uint32_t extent, float delight, CompositeScratch& scratch);

        /// Moved and never copied, like every other description that hands out spans of itself: two
        /// composites holding the same texels under one key is two answers to a question with one.
        TerrainComposite(const TerrainComposite&) = delete;
        TerrainComposite& operator=(const TerrainComposite&) = delete;
        TerrainComposite(TerrainComposite&&) noexcept = default;
        TerrainComposite& operator=(TerrainComposite&&) noexcept = default;

        /// The baked image, spanning storage this object owns and carrying a neutral shading map.
        /// `mSlot` and `mName` are the caller's to fill: the scene decides where a composite goes
        /// and what key found it.
        TextureData describe() const;

        std::uint32_t getLevelCount() const { return mTexture.getShape().getLevelCount(); }

    private:
        /// Reduces the summed light to the chain of encoded bytes a backend uploads, spending it.
        void buildChain(std::uint32_t extent, CompositeScratch& scratch);

        OwnedTexture mTexture;
    };
}
