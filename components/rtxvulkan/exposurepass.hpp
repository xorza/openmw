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

    /// How bright the frame is, as the one number the display curve scales it by. Two dispatches,
    /// because the reduction has to see every pixel's bin before it can divide by the total. It
    /// measures the image the curve is about to map, which is the upscaled one wherever something
    /// upscales. Two buffers, because a picture inside the interface is mapped at one and has no
    /// past (`getPictureExposure`).
    class ExposurePass
    {
    public:
        ExposurePass(const Device& device, const std::filesystem::path& shaderDirectory);

        /// Measures `frame` and moves the answer toward it at a rate in seconds, where `getExposure`
        /// points, because a brightness that was a pure function of the frame turned every
        /// one-frame excursion in the histogram into one in the whole image.
        ///
        /// @param frame the finished frame in linear radiance, in `VK_IMAGE_LAYOUT_GENERAL`.
        /// @param elapsedSeconds since the previous frame.
        /// @param reset true where there is no previous exposure to move from — the first frame, and
        ///        any frame the renderer was told has no past. The measurement is taken outright.
        void record(VkCommandBuffer commands, const Image& frame, float elapsedSeconds, bool reset, float bias) const;

        /// Holds the frame's exposure at `value` instead, measuring nothing — what a pixel test and
        /// a converged reference are built at. The frame's buffer either way, so the curve never
        /// learns which it got.
        void recordFixed(VkCommandBuffer commands, float value) const;

        /// One float, written by whichever of the two calls above ran.
        VkBuffer getExposure() const { return mExposure.getHandle(); }

        /// One float holding one, which is what a picture inside the interface is mapped at. Its
        /// own buffer, or every local-map tile a cell arrives with would throw the frame's
        /// adaptation away and the brightness would step at every cell boundary.
        VkBuffer getPictureExposure() const { return mPicture.getHandle(); }

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

        /// The frame's, which every measurement reads before it writes.
        Buffer mExposure;

        /// Host memory written once and never again, so a picture costs no write and no barrier.
        Buffer mPicture;
    };
}
