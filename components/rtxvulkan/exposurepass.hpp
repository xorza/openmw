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
    ///
    /// **Two buffers, because two things are mapped and only one of them has a past.** The frame's
    /// carries the eye from frame to frame; a picture inside the interface is mapped at one and is
    /// traced between two frames. `getPictureExposure` says what one buffer for both cost.
    class ExposurePass
    {
    public:
        ExposurePass(const Device& device, const std::filesystem::path& shaderDirectory);

        /// Measures `frame` and moves the answer toward it, where `getExposure` points.
        ///
        /// **What is written is not what was measured.** The exposure carries between frames and
        /// approaches the measurement at a rate in seconds, because adaptation is a time-domain
        /// thing and a brightness that was a pure function of the frame on screen turned every
        /// one-frame excursion in the histogram into a one-frame excursion in the whole image.
        ///
        /// @param frame the finished frame in linear radiance, in `VK_IMAGE_LAYOUT_GENERAL`.
        /// @param elapsedSeconds since the previous frame. A run that alternated measured frames
        ///        with fixed ones would want the time since the previous *measurement* instead, and
        ///        nothing does: a fixed exposure is pinned for a whole run or not at all.
        /// @param reset true where there is no previous exposure to move from — the first frame, and
        ///        any frame the renderer was told has no past. The measurement is taken outright.
        void record(VkCommandBuffer commands, const Image& frame, float elapsedSeconds, bool reset, float bias) const;

        /// Holds the frame's exposure at `value` instead, measuring nothing.
        ///
        /// **The frame's buffer either way**, so the curve never learns which it got. A fixed
        /// exposure is what a pixel test and a converged reference are built at: a measured one
        /// makes every expected value depend on the whole frame's histogram, which is not a number
        /// anybody can hand-compute.
        void recordFixed(VkCommandBuffer commands, float value) const;

        /// One float, written by whichever of the two calls above ran.
        VkBuffer getExposure() const { return mExposure.getHandle(); }

        /// One float holding one, which is what a picture inside the interface is mapped at.
        ///
        /// **Its own buffer, because a picture is traced between two frames and the frame's buffer
        /// is where the eye stands.** Written into that one, a picture's one is what the next
        /// frame's reduction reads back as the brightness it had adapted to — so every local-map
        /// tile a cell arrives with throws the adaptation away and starts it again from one, and
        /// the frame's brightness steps at every cell boundary.
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
