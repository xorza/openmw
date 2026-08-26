#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "shaders/scene.h"

namespace Rtx
{
    struct TextureData;

    /// An estimate of the light a texture already has painted into it.
    ///
    /// **Morrowind's textures were lit before they were saved.** Ambient occlusion in the corners,
    /// a highlight along a rim, the glow a lamp casts on the wall behind it — all of it is in the
    /// texels, because the engine they were drawn for could not put it there any other way. A ray
    /// tracer then lights them a second time, and the result is a room whose corners are dark twice
    /// over and whose lamps have a halo the light itself did not make.
    ///
    /// What this holds is the low-frequency part of a texture's brightness, as a factor to divide
    /// out. Two properties make that safe to do:
    ///
    /// - **It is normalised to average one**, so it moves light around a texture and never changes
    ///   how much of it there is.
    /// - **It is clamped**, so a texture whose darkest corner is genuinely black — paint rather than
    ///   shadow — is dimmed and brightened by at most a factor of two either way.
    ///
    /// **Coarse on purpose.** Painted lighting varies slowly across a surface and painted detail
    /// does not, so a grid this size follows the first and cannot follow the second: a checkerboard
    /// alternating every texel averages flat in every cell and comes back neutral. Following detail
    /// is the over-correction that flattens a texture into a colour.
    class ShadingMap
    {
    public:
        /// Cells along each edge of the grid the estimate is made on, which the shader indexes
        /// with and so declares.
        static constexpr std::uint32_t sExtent = Shaders::SHADING_EXTENT;

        /// How far the correction may reach, either way.
        static constexpr float sFloor = 0.5f;
        static constexpr float sCeiling = 2.0f;

        /// A map that changes nothing, which is what a texture that would not load has to get.
        ///
        /// The alternative is no map at all, and a shader reading a missing one reads whatever the
        /// array's stand-in holds — which is how every untextured surface in the reference
        /// implementation came to be divided by two.
        ShadingMap();

        /// Estimates the map from the texture's largest level.
        ///
        /// Block-compressed formats are read through their palettes rather than decompressed: a
        /// block's mean is its palette weighted by how many texels chose each entry, which is
        /// arithmetic on eight bytes and needs no decoder.
        explicit ShadingMap(const TextureData& texture);

        /// `sExtent * sExtent` factors, row by row, averaging one.
        std::span<const float> getValues() const { return mValues; }

    private:
        std::array<float, std::size_t{ sExtent } * sExtent> mValues;
    };

    /// The map at a point, bilinear across it and wrapping with it — the shader's `paintedLight`.
    ///
    /// Wrapping because Morrowind's textures tile and a great many of them rely on it: a map that
    /// clamped at its edges would put a seam down every wall that repeats.
    ///
    /// @param map `ShadingMap::sExtent` squared factors, which is what `TextureData::mShading` holds.
    float paintedLight(std::span<const float> map, float u, float v);
}
