#pragma once

#include <cstddef>
#include <cstdint>

namespace Rtx
{
    /// What a texture's texels are: a colour, which the file holds display-encoded and the hardware
    /// decodes inside the filter, or data, which is read as it is stored. A normal map is data: its
    /// channels are a direction, and the sRGB curve under it would bend every normal toward one
    /// corner. The file does not say which it is; what a surface binds it as does, so the texture
    /// table keys a slot on it as it keys one on the wrap.
    enum class TextureEncoding : std::uint8_t
    {
        Colour = 0,
        Data = 1,
    };

    inline constexpr std::size_t sTextureEncodingCount = 2;
}
