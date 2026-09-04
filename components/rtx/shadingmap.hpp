#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <osg/Vec3f>

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
        static constexpr std::size_t sCells = std::size_t{ sExtent } * sExtent;

        /// How far the correction may reach, either way.
        static inline const float sFloor = Shaders::SHADING_FLOOR;
        static inline const float sCeiling = Shaders::SHADING_CEILING;

        /// A map that changes nothing, which is what a texture that would not load has to get.
        ///
        /// The alternative is no map at all, and a shader reading a missing one reads whatever the
        /// array's stand-in holds — which is how every untextured surface in the reference
        /// implementation came to be divided by two.
        ShadingMap();

        /// Estimates the map from the texture's largest level.
        explicit ShadingMap(const TextureData& texture);

        /// `sExtent * sExtent` factors, row by row, averaging one.
        std::span<const float> getValues() const { return mValues; }

    private:
        std::array<float, sCells> mValues;
    };

    /// One factor as the device stores it: a sixteen-bit unorm over the map's own range, so that
    /// the neutral factor is exact and a step is a part in forty thousand. `SHADING_FLOOR` says why
    /// the range is the map's and not the format's.
    std::uint16_t encodeShading(float value);
    float decodeShading(std::uint16_t stored);

    /// A whole map as the device stores it, and the neutral map where `map` is empty.
    ///
    /// **A missing map has to be neutral rather than absent.** A material whose texture would not
    /// load still reads a map at its slot, and a descriptor bound to nothing is undefined at the
    /// dispatch; ones are what changes nothing, and this is where a backend gets them.
    std::array<std::uint16_t, ShadingMap::sCells> encodeShadingMap(std::span<const float> map);

    /// The map at a point, bilinear across it and wrapping with it — the shader's `paintedLight`.
    ///
    /// Wrapping because Morrowind's textures tile and a great many of them rely on it: a map that
    /// clamped at its edges would put a seam down every wall that repeats.
    ///
    /// @param map `ShadingMap::sExtent` squared factors, which is what `TextureData::mShading` holds.
    float paintedLight(std::span<const float> map, float u, float v);
}
