#pragma once

#include <array>
#include <cstdint>
#include <filesystem>

#include <vulkan/vulkan_core.h>

#include <components/rtx/shaders/digest.h>

#include "buffer.hpp"
#include "computepipeline.hpp"

namespace Rtx
{
    class Device;
    class GpuTimer;
    class Image;

    /// The frame's images folded into `FrameDigest`'s words on the device — `shaders/digest.h`.
    /// One dispatch over one shader, whatever the images' formats, because the load converts from
    /// each view's and the digest takes the bits of what came out.
    class DigestPass
    {
    public:
        /// What `record` copies into its buffer, which is what that buffer has to have room for.
        static constexpr VkDeviceSize sBytes = sizeof(std::uint32_t) * Shaders::DIGEST_IMAGES * Shaders::DIGEST_LANES;

        DigestPass(const Device& device, const std::filesystem::path& shaderDirectory);

        /// `images` into `into`, image `i` filling the `DIGEST_LANES` words from `i * DIGEST_LANES`,
        /// ordered for a host read last. Every image is at one extent and readable as a storage
        /// image in `GENERAL`, which the frame's are by the time the trace has handed its
        /// channels over.
        void record(VkCommandBuffer commands, const std::array<const Image*, Shaders::DIGEST_IMAGES>& images,
            const Buffer& into, GpuTimer* timer) const;

    private:
        ComputePipeline mPipeline;

        /// Where the words are folded, in the device's own memory, and copied out of once:
        /// folded straight into the frame's read-back buffer, every atomic was a transaction across
        /// the bus, and the pass took twenty-four milliseconds a frame. One, and not one a frame in
        /// flight, because the copy out of it stands in the queue before the next frame's clear.
        Buffer mLanes;
    };
}
