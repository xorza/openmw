#pragma once

#include <cstdint>
#include <filesystem>

#include <vulkan/vulkan_core.h>

#include <components/rtx/shaders/camera.h>

#include "computepipeline.hpp"
#include "image.hpp"

namespace Rtx
{
    class Device;
    class GBuffer;

    /// The denoiser: a few edge-stopping wavelet levels over the indirect channel, borrowing
    /// samples sideways from neighbours on the same surface because there is no time for enough
    /// bounces per pixel. Legitimate because the trace demodulated: this filters light, and the
    /// texture is multiplied back in afterwards. The sky, water and fog were resolved into
    /// `direct` and pass this by.
    class AtrousPass
    {
    public:
        AtrousPass(const Device& device, const std::filesystem::path& shaderDirectory);

        /// The other half of the ping-pong for a frame this size: the levels alternate, so a
        /// filtered frame needs a second channel to land in, the format of the one it takes turns
        /// with. A chain's, like the history, because the pass is shared and the image is not.
        static Image makeScratch(const Device& device, std::uint32_t width, std::uint32_t height);

        /// Runs every level and returns the channel the result ended up in, because the levels
        /// alternate and a copy back would be bandwidth spent on tidiness.
        ///
        /// @param buffer handed over, so the trace's writes are visible. Only the guide and the
        ///        depth are read from it.
        /// @param blended what the accumulator made of this frame's bounce, and the variance that
        ///        turns a difference in brightness into an edge or into noise: the first level's
        ///        input, written by the odd-numbered levels.
        /// @param history what the first level writes and the accumulator finds as its mean next
        ///        frame. `AccumulateHistory::getHistory` says why the feedback belongs here.
        /// @param scratch `makeScratch`'s, at least the camera's extent.
        /// @param camera the one the frame was traced with; the edge tests rebuild its rays.
        const Image& record(VkCommandBuffer commands, const GBuffer& buffer, const Image& blended, const Image& history,
            const Image& scratch, const Shaders::Camera& camera) const;

    private:
        ComputePipeline mPipeline;
    };
}
