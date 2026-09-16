#pragma once

#include <cstddef>
#include <cstdint>

namespace Rtx
{
    /// How a texture is addressed past its edges, along `s` and along `t`, as the content file
    /// states it on the texture and not on the image. Two bits, because the four combinations are
    /// what a sampler is made for; `Repeat` is what nearly everything states, and `Clamp` is what
    /// every banner, flag, tapestry and torch flame in the game states, so that its sheet's edge
    /// stops at the edge rather than bleeding the opposite one through the filter.
    enum class TextureWrap : std::uint8_t
    {
        Repeat = 0,
        ClampS = 1,
        ClampT = 2,
        Clamp = 3,
    };

    inline constexpr std::size_t sTextureWrapCount = 4;

    inline constexpr TextureWrap textureWrapOf(const bool clampS, const bool clampT)
    {
        return static_cast<TextureWrap>((clampS ? 1u : 0u) | (clampT ? 2u : 0u));
    }

    inline constexpr bool clampsS(const TextureWrap wrap)
    {
        return (static_cast<std::uint8_t>(wrap) & 1u) != 0u;
    }

    inline constexpr bool clampsT(const TextureWrap wrap)
    {
        return (static_cast<std::uint8_t>(wrap) & 2u) != 0u;
    }
}
