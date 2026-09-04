#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_FOGVOLUME_H
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_FOGVOLUME_H

#ifdef __cplusplus
#include <cstdint>

#include <vulkan/vulkan_core.h>

#define FOG_VOLUME_FORMAT VK_FORMAT_R16G16B16A16_SFLOAT
#define FOG_COVERAGE_FORMAT VK_FORMAT_R16_SFLOAT
#define FOG_DIRECTIONAL_FORMAT VK_FORMAT_R16G16_SFLOAT
#define FOG_DEPTH_FORMAT VK_FORMAT_R32_SFLOAT

namespace Rtx::FogBindings
{
    using uint = std::uint32_t;
#else
#define FOG_VOLUME_FORMAT rgba16f
#define FOG_COVERAGE_FORMAT r16f
#define FOG_DIRECTIONAL_FORMAT rg16f
#define FOG_DEPTH_FORMAT r32f
#endif

    const uint BIND_FOG_WAS_COVERAGE = 0;
    const uint BIND_FOG_WAS_VISIBILITY = 1;
    const uint BIND_FOG_COVERAGE = 2;
    const uint BIND_FOG_VISIBILITY = 3;
    const uint BIND_FOG_LAMPS = 4;
    const uint BIND_FOG_SLICE = 5;
    const uint BIND_FOG_SLICE_VISIBILITY = 6;
    const uint BIND_FOG_COVERAGE_TARGET = 7;
    const uint BIND_FOG_VISIBILITY_TARGET = 8;
    const uint BIND_FOG_LAMPS_TARGET = 9;
    const uint BIND_FOG_SLICE_TARGET = 10;
    const uint BIND_FOG_SLICE_VISIBILITY_TARGET = 11;
    const uint BIND_FOG_COLUMN_DEPTH = 12;
    const uint FOG_SAMPLED_COUNT = BIND_FOG_COVERAGE_TARGET;
    const uint FOG_BINDING_COUNT = BIND_FOG_COLUMN_DEPTH + 1;

#ifdef __cplusplus
}
#endif

#endif
