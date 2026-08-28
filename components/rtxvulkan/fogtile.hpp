#pragma once

#include <vulkan/vulkan_core.h>

#include "image.hpp"

namespace Rtx
{
    class CommandPool;
    class Device;

    /// The fog's fractal field, on the device.
    ///
    /// **Drawn once for the run and never again.** Nothing about it turns on the weather, the hour or
    /// the cell — what those decide is the extinction, the layer's height and how much of the band is
    /// cut, and every one of those is a number the shader already has. So this is built where the
    /// device is and held for as long as it, which is what makes a field this rich affordable at all.
    ///
    /// `Rtx::bakeFogNoise` says what is in it and why every level carries one spread.
    class FogTile
    {
    public:
        /// @param pool submits the one upload and waits for it. Not on the frame path.
        FogTile(const Device& device, CommandPool& pool);
        ~FogTile();

        FogTile(const FogTile&) = delete;
        FogTile& operator=(const FogTile&) = delete;

        /// The shape a coverage band is cut out of, and a second field decorrelated from it.
        const Image& getField() const { return mField; }

        /// Linear, mipmapped and wrapping — the field is laid down every tile, and a tap that
        /// clamped would smear the last texel of one across the whole landscape.
        VkSampler getSampler() const { return mSampler; }

    private:
        const Device& mDevice;
        Image mField;
        VkSampler mSampler = VK_NULL_HANDLE;
    };
}
