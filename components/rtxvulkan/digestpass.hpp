#pragma once

#include <array>
#include <filesystem>

#include <vulkan/vulkan_core.h>

#include <components/rtx/shaders/digest.h>

#include "computepipeline.hpp"

namespace Rtx
{
    class Buffer;
    class Device;
    class GpuTimer;
    class Image;

    /// The frame's images folded into `FrameDigest`'s words on the device — `shaders/digest.h`.
    /// One dispatch over one shader, whatever the images' formats, because the load converts from
    /// each view's and the digest takes the bits of what came out.
    class DigestPass
    {
    public:
        DigestPass(const Device& device, const std::filesystem::path& shaderDirectory);

        /// `images` into `lanes`, image `i` filling the `DIGEST_LANES` words from `i * DIGEST_LANES`;
        /// the buffer is cleared first and ordered for a host read last. Every image is at one
        /// extent and readable as a storage image in `GENERAL`, which the frame's are by the time
        /// the trace has handed its channels over.
        void record(VkCommandBuffer commands, const std::array<const Image*, Shaders::DIGEST_IMAGES>& images,
            const Buffer& lanes, GpuTimer* timer) const;

    private:
        ComputePipeline mPipeline;
    };
}
