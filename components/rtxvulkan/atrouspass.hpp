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

    /// The denoiser: a few edge-stopping wavelet levels over the indirect channel.
    ///
    /// **One bounce per pixel is an unbiased estimate and a terrible picture.** The average of
    /// enough of them is the answer, and there is no time for enough of them, so the samples that
    /// exist are borrowed sideways from neighbours that are looking at the same surface. What makes
    /// that legitimate is the demodulation the trace already did: this filters light, and the
    /// texture it lands on is multiplied back in afterwards, unblurred.
    ///
    /// It touches nothing else. The sky, water and everything the fog laid over the frame were
    /// resolved into `direct` by the trace and pass this by, which is why the filter needs to know
    /// nothing about any of them.
    class AtrousPass
    {
    public:
        AtrousPass(const Device& device, const std::filesystem::path& shaderDirectory);

        AtrousPass(const AtrousPass&) = delete;
        AtrousPass& operator=(const AtrousPass&) = delete;

        /// Makes room for a frame this size, if the last one was not. Before the first frame.
        ///
        /// The levels ping-pong, so a filtered frame needs a second channel to land in. Idempotent,
        /// and the caller is expected to have waited for anything still reading the old one.
        void resize(std::uint32_t width, std::uint32_t height);

        /// Runs every level and returns the channel the result ended up in.
        ///
        /// **Returned rather than promised**, because the levels alternate and where they finish
        /// depends on how many there are. The alternative is a full-frame copy to put the answer
        /// back where the caller assumed it would be, which is bandwidth spent on tidiness.
        ///
        /// @param buffer must have been handed over, so the trace's writes are visible here. Its
        ///        indirect channel is the input and may be written by an even-numbered level.
        /// @param moments what the accumulator in front of this measured: the estimator's own
        ///        variance, which is what turns a difference in brightness into an edge or into
        ///        noise. A pixel with no history carries one, and one means "filter widely".
        /// @param camera the one the frame was traced with; the edge tests rebuild its rays.
        const Image& record(
            VkCommandBuffer commands, const GBuffer& buffer, const Image& moments, const Shaders::Camera& camera) const;

    private:
        const Device& mDevice;
        ComputePipeline mPipeline;

        /// The other half of the ping-pong, the size of a frame and the format of the channel it
        /// takes turns with. Null until `resize`.
        std::unique_ptr<Image> mScratch;
    };
}
