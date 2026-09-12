#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

#include <vulkan/vulkan_core.h>

#include <components/rtx/shaders/atrous.h>

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

        /// Makes room for a frame this size, if the last one was not. Before the first frame.
        ///
        /// The levels ping-pong, so a filtered frame needs a second channel to land in. Idempotent,
        /// and the caller is expected to have waited for anything still reading the old one.
        void resize(std::uint32_t width, std::uint32_t height);

        /// Runs every level and returns the channel the result ended up in, because the levels
        /// alternate and a copy back would be bandwidth spent on tidiness.
        ///
        /// @param buffer handed over, so the trace's writes are visible. Only the guide and the
        ///        depth are read from it.
        /// @param blended what the accumulator made of this frame's bounce: the first level's
        ///        input, written by the odd-numbered levels.
        /// @param moments the estimator's own variance, which turns a difference in brightness into
        ///        an edge or into noise. A pixel with no history carries one, which filters widely.
        /// @param history what the first level writes and the accumulator finds as its mean next
        ///        frame. `AccumulatePass::getHistory` says why the feedback belongs here.
        /// @param camera the one the frame was traced with; the edge tests rebuild its rays.
        const Image& record(VkCommandBuffer commands, const GBuffer& buffer, const Image& blended, const Image& moments,
            const Image& history, const Shaders::Camera& camera) const;

    private:
        const Device& mDevice;
        ComputePipeline mPipeline;

        /// The other half of the ping-pong, the size of a frame and the format of the channel it
        /// takes turns with. Null until `resize`.
        std::unique_ptr<Image> mScratch;
    };
}
