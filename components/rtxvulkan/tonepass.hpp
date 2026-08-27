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

    /// Scene-referred radiance to bytes a display understands, and the sky's own points over it.
    ///
    /// **The last pass, and the only one that knows what a display is.** Everything before it works
    /// in linear radiance — including the upscaler, which reconstructs from several frames of it —
    /// so the curve runs once, at the end, over whatever resolution the frame reached by then.
    ///
    /// **Which is also the only place a point source can be drawn.** A star is sub-pixel and high
    /// contrast, and a temporal upscaler is built to remove exactly that; the resolution the frame
    /// is shown at is the last one in the frame, so this pass has it and no other does.
    /// `ToneConstants::mStars` carries the measurements.
    class TonePass
    {
    public:
        /// @param textureLayout the scene's bindless textures, which this samples the star sheet
        ///        out of — `ToneConstants::mStars` says why the field is drawn here.
        TonePass(
            const Device& device, VkDescriptorSetLayout textureLayout, const std::filesystem::path& shaderDirectory);

        TonePass(const TonePass&) = delete;
        TonePass& operator=(const TonePass&) = delete;

        /// @param colour the finished frame in linear radiance, in `VK_IMAGE_LAYOUT_GENERAL`.
        /// @param exposure one float, what to scale it by. `ExposurePass` writes it, measured off
        ///        this same image or fixed, and this pass never learns which.
        /// @param starsShown what the star field has to be drawn through, in
        ///        `VK_IMAGE_LAYOUT_GENERAL`, at the extent the trace ran at. `GBuffer::getStarsShown`
        ///        says why this pass cannot work it out for itself.
        /// @param textures the scene's texture descriptor set, bound as set one.
        /// @param constants how much of the target to encode from its top-left corner — the whole of
        ///        it for a frame, and a corner of it for a picture inside the interface, which fills
        ///        as much of a texture as its widget is currently wide — beside the camera on that
        ///        grid, the trace's own extent, and the star field to draw.
        /// @param target the displayable image, in `VK_IMAGE_LAYOUT_GENERAL`.
        void record(VkCommandBuffer commands, const Image& colour, VkBuffer exposure, const Image& starsShown,
            VkDescriptorSet textures, const Image& target, const Shaders::ToneConstants& constants) const;

    private:
        ComputePipeline mPipeline;
    };
}
