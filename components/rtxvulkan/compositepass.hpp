#pragma once

#include <filesystem>

#include <vulkan/vulkan_core.h>

#include <components/rtx/shaders/composite.h>

#include "computepipeline.hpp"
#include "image.hpp"

namespace Rtx
{
    class CommandPool;
    class Device;
    class GBuffer;

    /// Puts the trace's channels back together and hands over one frame of linear radiance.
    ///
    /// **One multiply and one add** — everything harder was folded into the modulation term by the
    /// trace, which is what lets a filter sit in between knowing nothing about water or fog. It also
    /// owns the running sum, because what a reference has to converge to is the frame as it will be
    /// shown, filter and all.
    ///
    /// The display curve is not here. It is `TonePass`, after whatever upscales.
    class CompositePass
    {
    public:
        /// @param pool used once, to lay out the stand-in below. Nothing here touches it again.
        CompositePass(const Device& device, CommandPool& pool, const std::filesystem::path& shaderDirectory);

        CompositePass(const CompositePass&) = delete;
        CompositePass& operator=(const CompositePass&) = delete;

        /// @param buffer must have been handed over, so its writes are visible to this read. Its
        ///        indirect channel is not read: `indirect` says where the bounce actually is.
        /// @param indirect the bounce to put the albedo back into — the filter's output, or the
        ///        buffer's own channel where nothing filtered it. Whose image it is depends on how
        ///        many wavelet levels ran, which is why it is named rather than assumed.
        /// @param history the running sum, at least as large as `target` and in
        ///        `VK_IMAGE_LAYOUT_GENERAL`. Null where `mAccumulate` is zero, which is every frame
        ///        that is not building a reference.
        /// @param colour the recombined frame, in `VK_IMAGE_LAYOUT_GENERAL` and in linear radiance.
        void record(VkCommandBuffer commands, const GBuffer& buffer, const Image& indirect, const Image* history,
            const Image& colour, const Shaders::CompositeConstants& constants) const;

    private:
        ComputePipeline mPipeline;

        /// What the history binding points at when there is no history.
        ///
        /// **A descriptor has to point somewhere and this one is never read.** The shader touches
        /// the sum only inside `if (mAccumulate > 0u)`, so a caller that never averages would
        /// otherwise carry a full-size float image for a binding nothing looks at — sixteen bytes a
        /// pixel, which is 133 MiB at 4K. One texel does the same job.
        Image mNoHistory;
    };
}
