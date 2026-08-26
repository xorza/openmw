#pragma once

#include <filesystem>

#include <vulkan/vulkan_core.h>

#include <components/rtx/shaders/exposure.h>

#include "buffer.hpp"
#include "computepipeline.hpp"

namespace Rtx
{
    class Device;
    class Image;

    /// How bright the frame is, as the one number the display curve scales it by.
    ///
    /// **Two dispatches and not one.** The first bins every pixel by log luminance; the second
    /// reduces the bins to a single value. They cannot be merged, because the reduction has to see
    /// every pixel's contribution before it can divide by the total, and a dispatch boundary is the
    /// only barrier wide enough to promise that.
    ///
    /// **It measures the image the curve is about to map**, which is the upscaled one wherever
    /// something upscales — see `histogram.comp` for what measuring the other one costs. Both are
    /// bound from one source and dispatched over one extent so the two cannot come apart.
    class ExposurePass
    {
    public:
        ExposurePass(const Device& device, const std::filesystem::path& shaderDirectory);

        ExposurePass(const ExposurePass&) = delete;
        ExposurePass& operator=(const ExposurePass&) = delete;

        /// Measures `frame` and leaves the answer where `getExposure` points.
        ///
        /// @param frame the finished frame in linear radiance, in `VK_IMAGE_LAYOUT_GENERAL`.
        void record(VkCommandBuffer commands, const Image& frame) const;

        /// Writes `value` there instead, measuring nothing.
        ///
        /// **The same buffer either way**, so the curve never learns which it got. A fixed exposure
        /// is what a pixel test and a converged reference are built at: a measured one makes every
        /// expected value depend on the whole frame's histogram, which is not a number anybody can
        /// hand-compute.
        void recordFixed(VkCommandBuffer commands, float value) const;

        /// One float, written by whichever of the two calls above ran.
        VkBuffer getExposure() const { return mExposure.getHandle(); }

    private:
        /// Orders the previous frame's reads against the writes about to replace them.
        void beforeWrite(VkCommandBuffer commands) const;

        /// Orders whatever wrote the buffer against the pass about to read it.
        void handOver(VkCommandBuffer commands) const;

        ComputePipeline mHistogramPipeline;
        ComputePipeline mReducePipeline;

        /// One `uint` per bin, cleared at the start of every measurement — a shader that cleared it
        /// would race with the workgroups already accumulating into it.
        Buffer mHistogram;

        Buffer mExposure;
    };
}
