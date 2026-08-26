#pragma once

#include <cstdint>
#include <filesystem>

#include <vulkan/vulkan_core.h>

#include <components/rtx/shaders/tone.h>

#include "computepipeline.hpp"

namespace Rtx
{
    class Device;
    class Image;

    /// Scene-referred radiance to bytes a display understands, and nothing else.
    ///
    /// **The last pass, and the only one that knows what a display is.** Everything before it works
    /// in linear radiance — including the upscaler, which reconstructs from several frames of it —
    /// so the curve runs once, at the end, over whatever resolution the frame reached by then.
    class TonePass
    {
    public:
        TonePass(const Device& device, const std::filesystem::path& shaderDirectory);

        TonePass(const TonePass&) = delete;
        TonePass& operator=(const TonePass&) = delete;

        /// @param colour the finished frame in linear radiance, in `VK_IMAGE_LAYOUT_GENERAL`.
        /// @param exposure one float, what to scale it by. `ExposurePass` writes it, measured off
        ///        this same image or fixed, and this pass never learns which.
        /// @param target the displayable image, in `VK_IMAGE_LAYOUT_GENERAL`.
        /// @param width, height how much of it to encode, from the top-left corner. The whole of it
        ///        for a frame — which is the **target's** size and not the colour's, because with an
        ///        upscaler between them the two differ — and a corner of it for a picture inside the
        ///        interface, which fills as much of a texture as its widget is currently wide.
        void record(VkCommandBuffer commands, const Image& colour, VkBuffer exposure, const Image& target,
            std::uint32_t width, std::uint32_t height) const;

    private:
        ComputePipeline mPipeline;
    };
}
