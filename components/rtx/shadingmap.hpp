#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <osg/Vec3f>

#include "shaders/look.h"
#include "shaders/scene.h"

namespace Rtx
{
    struct TextureData;

    /// An estimate of the light a texture already has painted into it. Morrowind's textures were
    /// lit before they were saved, so a ray tracer lights their corners dark twice over. This is
    /// the low-frequency part of a texture's brightness as a factor to divide out: normalised to
    /// average one, so it moves light around and never changes how much there is, and clamped to
    /// a factor of two, so paint that is black stays near black. Coarse on purpose, because
    /// painted lighting varies slowly and painted detail does not, and following the detail is the
    /// over-correction that flattens a texture into a colour.
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

        /// A map that changes nothing, which is what a texture that would not load has to get: a
        /// shader reading a missing one reads the array's stand-in, which is how every untextured
        /// surface in the reference implementation came to be divided by two.
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

    /// A whole map as the device stores it, and the neutral map where `map` is empty, because a
    /// material whose texture would not load still reads a map at its slot.
    std::array<std::uint16_t, ShadingMap::sCells> encodeShadingMap(std::span<const float> map);

    /// The map at a point, bilinear across it and wrapping with it — the shader's `paintedLight`.
    /// Wrapping because Morrowind's textures tile, and a clamp would seam every wall that repeats.
    ///
    /// @param map `ShadingMap::sExtent` squared factors, which is what `TextureData::mShading` holds.
    float paintedLight(std::span<const float> map, float u, float v);
}
