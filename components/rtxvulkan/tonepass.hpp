#pragma once

#include <filesystem>

#include <vulkan/vulkan_core.h>

#include <components/rtx/shaders/tone.h>

#include "buffer.hpp"
#include "computepipeline.hpp"
#include "handles.hpp"
#include "image.hpp"

namespace Rtx
{
    class Device;

    /// What one run of the curve is over. A frame's and a picture's inside the interface differ
    /// in the pyramid and the sun's share and in nothing else, so the two are one record and not
    /// two argument lists — four of the fields below are an `Image`, and a list of them takes any
    /// two of the four in either order.
    struct Tone
    {
        /// The finished frame in linear radiance, in `VK_IMAGE_LAYOUT_GENERAL`.
        const Image& mColour;

        /// One float, what to scale it by. `ExposurePass` writes it, measured off `mColour` or
        /// fixed, and this pass never learns which.
        const Buffer& mExposure;

        /// One float, how much of the sun's quad the eye could see — the frame's
        /// `SunGlarePass::getShare`, or its `getNoShare` for a picture inside the interface.
        const Buffer& mSunGlare;

        /// What the star field has to be drawn through, in `VK_IMAGE_LAYOUT_GENERAL`, at the
        /// extent the trace ran at. `GBuffer::getStarsShown` says why this pass cannot work it
        /// out for itself.
        const Image& mStarsShown;

        /// The trace's own depth of the puffs, at the same extent.
        const Image& mPuffsDepth;

        /// The pyramid's finest level, in `VK_IMAGE_LAYOUT_GENERAL`, or null where nothing built
        /// one — a doll, a map tile, a frame too small to halve.
        const Image* mBloom = nullptr;

        /// The scene's texture descriptor set, bound as set one.
        VkDescriptorSet mTextures = VK_NULL_HANDLE;

        /// The displayable image, in `VK_IMAGE_LAYOUT_GENERAL`.
        const Image& mTarget;

        /// How much of the target to encode from its top-left corner — a corner of it for a
        /// picture inside the interface — beside the camera, the trace's extent and the star
        /// field. `record` copies it and completes it from `mBloom`, so no caller can hand over
        /// a strength with no pyramid behind it.
        Shaders::ToneConstants mConstants;
    };

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
        TonePass(
            const Device& device, VkDescriptorSetLayout textureLayout, const std::filesystem::path& shaderDirectory);

        void record(VkCommandBuffer commands, const Tone& what) const;

    private:
        ComputePipeline mPipeline;

        /// Linear and clamped, which is what the tent the pyramid is spread with is counted in.
        Sampler mSampler;

        /// What binding four holds where there is no pyramid. `makeStandIn` says why.
        Image mNoBloom;
    };
}
