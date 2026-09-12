#pragma once

#include <cstdint>
#include <filesystem>

#include <vulkan/vulkan_core.h>

#include <components/rtx/shaders/tone.h>

#include "computepipeline.hpp"
#include "handles.hpp"
#include "image.hpp"

namespace Rtx
{
    class CommandPool;
    class Device;

    /// Scene-referred radiance to bytes a display understands, and the sky's own points over it.
    /// The last pass and the only one that knows what a display is, so the curve runs once over
    /// whatever resolution the frame reached. Also the only place a point source can be drawn,
    /// because a temporal upscaler is built to remove exactly a sub-pixel high-contrast star.
    /// `ToneConstants::mStars` carries the measurements.
    class TonePass
    {
    public:
        /// @param textureLayout the scene's bindless textures, which this samples the star sheet
        ///        out of — `ToneConstants::mStars` says why the field is drawn here.
        /// @param pool where the stand-in bound in place of a pyramid is put into its layout, once.
        TonePass(const Device& device, CommandPool& pool, VkDescriptorSetLayout textureLayout,
            const std::filesystem::path& shaderDirectory);

        /// @param colour the finished frame in linear radiance, in `VK_IMAGE_LAYOUT_GENERAL`.
        /// @param exposure one float, what to scale it by. `ExposurePass` writes it, measured off
        ///        this same image or fixed, and this pass never learns which.
        /// @param starsShown what the star field has to be drawn through, in
        ///        `VK_IMAGE_LAYOUT_GENERAL`, at the extent the trace ran at. `GBuffer::getStarsShown`
        ///        says why this pass cannot work it out for itself.
        /// @param textures the scene's texture descriptor set, bound as set one.
        /// @param constants how much of the target to encode from its top-left corner — a corner
        ///        of it for a picture inside the interface — beside the camera, the trace's extent
        ///        and the star field. Taken by value and completed here from `bloom`, so no caller
        ///        can hand over a strength with no pyramid behind it.
        /// @param bloom the pyramid's finest level, in `VK_IMAGE_LAYOUT_GENERAL`, or null where
        ///        nothing built one — a doll, a map tile, a frame too small to halve.
        /// @param target the displayable image, in `VK_IMAGE_LAYOUT_GENERAL`.
        void record(VkCommandBuffer commands, const Image& colour, VkBuffer exposure, const Image& starsShown,
            const Image* bloom, VkDescriptorSet textures, const Image& target, Shaders::ToneConstants constants) const;

    private:
        ComputePipeline mPipeline;

        /// Linear and clamped, which is what the tent the pyramid is spread with is counted in.
        Sampler mSampler;

        /// What binding four holds where there is no pyramid. `makeStandIn` says why.
        Image mNoBloom;
    };
}
