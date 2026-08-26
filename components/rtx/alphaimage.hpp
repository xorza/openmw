#pragma once

#include <cassert>
#include <cstdint>
#include <vector>

#include "texturedata.hpp"

namespace Rtx
{
    /// A texture's alpha channel, decoded to a byte a texel, at every level the file carried.
    ///
    /// **What a cutout is, separated from what it looks like.** Every other reader of a texture in
    /// this renderer wants its colour, and the two block formats that carry alpha keep it in eight
    /// bytes that `ColourBlock` deliberately steps over — so the one question a cutout asks, *is
    /// there anything here*, had nowhere to be answered. Opacity micromaps ask it a few hundred
    /// thousand times per mesh, which is what makes it worth decoding once into a plain array
    /// instead of unpacking a block per lookup.
    ///
    /// **Alpha is linear in every format.** The colours beside it are display-encoded and the
    /// sampler converts them on the way in; alpha never was, so nothing here has a transfer
    /// function in it and a byte means what it says.
    ///
    /// **The whole chain and not the largest level, because the shader reads the whole chain.**
    /// `alphaPasses` samples the mask at the level the ray's cone can resolve, so what a cutout
    /// test can return at a point is bounded by every level over it and not by the finest one — and
    /// a classifier that read only the finest would be answering a question the renderer never
    /// asks. `AlphaBounds` is what turns the chain into that bound.
    class AlphaImage
    {
    public:
        /// Decodes every level the description carries. A texture with none leaves this empty,
        /// which is a texture whose cutout could not be read — a caller that cannot answer for one
        /// has to leave it asking rather than decide on its behalf.
        explicit AlphaImage(const TextureData& texture);

        std::uint32_t getLevelCount() const { return static_cast<std::uint32_t>(mLevels.size()); }

        /// Where one level sits in the values, and how big it is. `MipLevel::mOffset` counts texels
        /// here rather than bytes, this being one byte a texel.
        const MipLevel& getLevel(std::uint32_t level) const { return mLevels[level]; }

        /// The finest level's extent, which is the space a micromap's footprints are measured in.
        std::uint32_t getWidth() const { return mLevels.empty() ? 0 : mLevels.front().mWidth; }
        std::uint32_t getHeight() const { return mLevels.empty() ? 0 : mLevels.front().mHeight; }

        bool isEmpty() const { return mLevels.empty(); }

        /// Alpha at a texel of a level, both of which must be inside the image.
        ///
        /// Defined here because `AlphaBounds` asks it nine times per texel per level, and a call
        /// across a translation unit for a vector index is most of what that walk costs.
        std::uint8_t at(std::uint32_t level, std::uint32_t x, std::uint32_t y) const
        {
            const MipLevel& which = mLevels[level];
            assert(x < which.mWidth && y < which.mHeight);

            return mValues[which.mOffset + std::size_t{ y } * which.mWidth + x];
        }

    private:
        std::vector<MipLevel> mLevels;
        std::vector<std::uint8_t> mValues;
    };
}
