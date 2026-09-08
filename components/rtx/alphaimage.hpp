#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "texturedata.hpp"

namespace osg
{
    class Image;
}

namespace Rtx
{
    /// A texture's alpha channel, decoded to a byte a texel, at every level the file carried.
    ///
    /// **What a cutout is, separated from what it looks like.** Every other reader of a texture in
    /// this renderer wants its colour, and the two block formats that carry alpha keep it in eight
    /// bytes that `ColourBlock` deliberately steps over — so the one question a cutout asks, *is
    /// there anything here*, had nowhere to be answered. `SpriteLightMap` walks every texel of every
    /// level to bake one, which is what makes it worth decoding once into a plain array instead of
    /// unpacking a block per lookup.
    ///
    /// **Alpha is linear in every format.** The colours beside it are display-encoded and the
    /// sampler converts them on the way in; alpha never was, so nothing here has a transfer
    /// function in it and a byte means what it says.
    ///
    /// **The whole chain and not the largest level, because the shader reads the whole chain.**
    /// A sprite is sampled at whatever level the ray's cone can resolve, so a bake made from the
    /// finest level alone would be wrong at every distance but one.
    class AlphaImage
    {
    public:
        AlphaImage() = default;

        /// For a caller with one texture to read and no image to reuse. `build` is the whole of it.
        explicit AlphaImage(const TextureData& texture) { build(texture); }

        /// Decodes every level the description carries. A texture with none leaves this empty,
        /// which is a texture whose cutout could not be read — a caller that cannot answer for one
        /// has to leave it asking rather than decide on its behalf.
        ///
        /// **Refills this one rather than making another**, so a loader that reads a cell's worth
        /// keeps the room the last texture grew. Whatever was here is gone, buffers apart.
        void build(const TextureData& texture);

        /// Where the levels sit in the values. `MipLevel::mOffset` counts texels here rather than
        /// bytes, this being one byte a texel.
        const MipPyramid& getShape() const { return mShape; }

        std::uint32_t getLevelCount() const { return mShape.getLevelCount(); }
        const MipLevel& getLevel(std::uint32_t level) const { return mShape.getLevel(level); }

        /// The largest level's extent, which is what a bake made from this is sized to.
        std::uint32_t getWidth() const { return mShape.getWidth(); }
        std::uint32_t getHeight() const { return mShape.getHeight(); }

        bool isEmpty() const { return mShape.isEmpty(); }

        /// Alpha at a texel of a level, both of which must be inside the image.
        ///
        /// Defined here because `SpriteLightMap` asks it for every texel of every level, and a call
        /// across a translation unit for a vector index is most of what that walk costs.
        std::uint8_t at(std::uint32_t level, std::uint32_t x, std::uint32_t y) const
        {
            return mValues[mShape.offsetOf(level, x, y, 1)];
        }

    private:
        MipPyramid mShape;
        std::vector<std::uint8_t> mValues;
    };

    /// The buffers `reachesSolid` reads an image through, held by whoever asks rather than made
    /// per call.
    ///
    /// **Once per translucent diffuse map a cell arrives with**, which `MaterialResolver`'s cache
    /// is what makes it: a material asks per surface and the answer is kept per image. Each of
    /// those readings is a levels table and a decoded alpha channel, and held they are the room the
    /// image before grew.
    struct AlphaScratch
    {
        std::vector<MipLevel> mLevels;
        AlphaImage mAlpha;
    };

    /// Whether any texel of `image` is fully opaque.
    ///
    /// **What tells a wisp from a mask, and it is a fact about the texture alone.** Morrowind keeps
    /// its foliage and its clouds under one alpha mode, so the mode says nothing: a leaf card is
    /// solid wherever its paint is, because the paint is a silhouette. A cloud's alpha is a gradient
    /// that never closes — `Tx_Dagoth_Cloud` peaks at seven fifteenths and `Tx_Dagoth_cloud02` at
    /// ten — and a surface that is nowhere opaque is not a surface. The eye passes through that and
    /// stops on the other.
    ///
    /// **The finest level alone, which is the only one that can answer.** Every coarser level is an
    /// average of the one above it, and a mask's average stops reaching solid a level or two down —
    /// so a chain read whole would call every leaf in the game a wisp. It is also the only one
    /// decoded.
    ///
    /// **True for an image nothing here can decode**, because a texture this cannot answer for is
    /// not one to turn into a volume on a guess.
    ///
    /// @param scratch what the reading is done in, which is the caller's. Cleared and refilled here,
    ///        and read by nothing afterwards.
    bool reachesSolid(const osg::Image& image, AlphaScratch& scratch);
}
