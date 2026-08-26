#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

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
    /// **Not on the frame path, and the number says why.** A chunk of a four-cell region costs
    /// **28.5 ms** to flatten, measured over the 73 of them Balmora produces at that radius. A frame
    /// that bakes one is a dropped frame however good the average is, and a cell boundary bringing
    /// eight is a quarter of a second — so the unit of incremental work a caller drains has to be
    /// smaller than a whole composite.
    ///
    /// It was twice that until the sampling below stopped decoding a compressed block at every tap;
    /// what is left is a quarter of a million output texels, each summing the stack.
    class TerrainComposite
    {
    public:
        /// Takes the stack on at `extent` square, and bakes none of it.
        ///
        /// **Everything the bake reads is copied in here**, which is what lets `bake` be spread over
        /// as many frames as the caller likes: each layer's diffuse is decoded to the two levels the
        /// whole bake will ever read, and its mask and painted light — a few hundred floats between
        /// them — are taken by value. Nothing it reads afterwards belongs to anyone else, so a chunk
        /// that leaves the world part way through a bake takes nothing out from under it.
        ///
        /// @param extent a power of two, so the chain below halves exactly and ends at one texel.
        /// @param delight how much of each layer's painted light to divide out, matching the frame
        ///        constant the shader reads. **Baked in rather than left to the shader**, because
        ///        the estimate repeats with the texture's tiling and the composite has none: this is
        ///        the last point at which the tiling is still known.
        TerrainComposite(std::span<const CompositeLayer> layers, std::uint32_t extent, float delight);

        ~TerrainComposite();

        /// Moved and never copied, like every other description that hands out spans of itself: two
        /// composites holding the same texels under one key is two answers to a question with one.
        TerrainComposite(const TerrainComposite&) = delete;
        TerrainComposite& operator=(const TerrainComposite&) = delete;
        TerrainComposite(TerrainComposite&&) noexcept;
        TerrainComposite& operator=(TerrainComposite&&) noexcept;

        /// Bakes at most `rows` more rows of the composite, and answers whether it is finished.
        ///
        /// **The unit a caller drains, and it has to be smaller than a whole composite.** One of
        /// these costs 28.5 ms; a frame that bakes one is a dropped frame however good the average
        /// is, and a cell boundary bringing eight is a quarter of a second. A row is the obvious
        /// slice because the sum is per output texel and the rows are independent.
        ///
        /// The mip chain is built by the call that finishes the last row, which is cheap beside it:
        /// a box filter over a third as many texels again, and no layer sampled at all.
        ///
        /// Calling this on a finished composite does nothing and says so.
        bool bake(std::uint32_t rows);

        /// Whether every row is summed and the chain built. **The chain is the record of it**:
        /// only the call that finishes the last row builds one, so there is no flag to disagree with.
        bool isDone() const;

        /// How many rows of the sum are baked, which is how a caller draining several composites
        /// knows what its budget went on.
        std::uint32_t getBakedRows() const;

        /// The baked image, spanning storage this object owns and carrying a neutral shading map.
        ///
        /// `mSlot` and `mName` are the caller's to fill: the scene decides where a composite goes
        /// and what key found it. Only a finished composite has anything to describe.
        TextureData describe() const;

        std::uint32_t getLevelCount() const;

    private:
        void buildChain();

        /// What the bake reads and writes, out of line so this header names none of it.
        struct Prepared;
        std::unique_ptr<Prepared> mPrepared;

        std::uint32_t mExtent = 0;
    };
}
