#pragma once

#include <cstdint>

#include <components/rtx/ownedtexture.hpp>
#include <components/rtx/texturedata.hpp>

namespace Rtx
{
    class AlphaImage;
}

namespace Rtx::Testing
{
    /// The host's statement of a sprite's light bake, which `SpriteLightMap` says the meaning of:
    /// what the device's bake, `SpriteLightPass`, is held to, texel for texel.
    class SpriteLightBake
    {
    public:
        /// Bakes every level `alpha` carries. An alpha with none leaves this empty.
        explicit SpriteLightBake(const AlphaImage& alpha);

        bool isEmpty() const { return mTexture.isEmpty(); }

        /// The bake as a backend uploads it: linear, four bytes a texel, every level. Spans this
        /// object's own storage.
        TextureData describe() const;

        /// One channel of one texel of one level, all of which must be inside the image.
        std::uint8_t at(std::uint32_t level, std::uint32_t x, std::uint32_t y, std::uint32_t channel) const;

    private:
        OwnedTexture mTexture;
    };
}
