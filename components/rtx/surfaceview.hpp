#pragma once

#include <array>
#include <cstdint>
#include <string_view>
#include <utility>

#include "namedenum.hpp"
#include "shaders/visibility.h"

namespace Rtx
{
    /// What a traced pixel is painted with: the light, or one input of the surface the light is
    /// computed from, written straight out. What a test asserting "this pixel is that texel" needs,
    /// and what makes a map problem visible as itself.
    enum class SurfaceView : std::uint32_t
    {
        Shaded = Shaders::SHOW_SHADED,

        /// The diffuse albedo.
        Albedo = Shaders::SHOW_ALBEDO,

        /// The shading normal, a normal map's included, as `0.5 + 0.5 n` in world space.
        Normal = Shaders::SHOW_NORMAL,

        /// The perceptual roughness, one on a surface with no specular map.
        Roughness = Shaders::SHOW_ROUGHNESS,

        /// The reflectance at normal incidence, `F0`: nought on a surface with no specular map, 4% on
        /// a dielectric that has one, and the base colour on a metal.
        Specular = Shaders::SHOW_SPECULAR,
    };

    inline constexpr NamedEnum sSurfaceViewNames{ std::array{
        std::pair{ SurfaceView::Shaded, std::string_view("shaded") },
        std::pair{ SurfaceView::Albedo, std::string_view("albedo") },
        std::pair{ SurfaceView::Normal, std::string_view("normal") },
        std::pair{ SurfaceView::Roughness, std::string_view("roughness") },
        std::pair{ SurfaceView::Specular, std::string_view("specular") },
    } };
}
