#pragma once

#include <array>
#include <cstdint>
#include <string_view>
#include <utility>

#include "namedenum.hpp"

namespace Rtx
{
    /// What a `_spec` map's channels mean. The file cannot say: OpenMW documents a classic layout
    /// (highlight colour in RGB, glossiness in A), the PBR packs this renderer is made to read use
    /// another, and one install can hold both. So the player states it, `[RTX] specular map layout`.
    enum class SpecularLayout : std::uint8_t
    {
        /// Read no specular map at all. What a classic map would come to as metalness and roughness
        /// is wrong, and no physical reading of a highlight colour as a reflectance exists.
        Ignore,

        /// R metalness, G perceptual roughness, B ambient occlusion, A one less subsurface scattering:
        /// the layout of Wareya's and Rafael's PBR shaders and of the packs made for them.
        MetalRoughness,
    };

    inline constexpr NamedEnum sSpecularLayoutNames{ std::array{
        std::pair{ SpecularLayout::Ignore, std::string_view("ignore") },
        std::pair{ SpecularLayout::MetalRoughness, std::string_view("metal roughness") },
    } };
}
