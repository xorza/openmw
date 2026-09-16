#pragma once

#include <cstdint>
#include <filesystem>

#include <vulkan/vulkan_core.h>

#include "buffer.hpp"
#include "computepipeline.hpp"

namespace Rtx
{
    class Device;
    class GpuTimer;

    /// A dispatch that holds the queue for a stated time and touches nothing a frame reads —
    /// `RendererOptions::mStressOverlapMs`. Appended to every frame's trace, it keeps the device
    /// that far behind the host, so every frame is recorded over a frame still running: a hazard
    /// that needs the overlap to show shows on the first frame of every run rather than on one run
    /// in four. The zone it is timed as is `stress`, so the report shows what it actually held.
    class StressPass
    {
    public:
        /// Calibrates the loop against this device: how many iterations a millisecond is, read off
        /// the device's own clock after enough dispatches for the card to have come off its idle
        /// clock. A stated time and not a stated count, because the count a millisecond takes is
        /// the card's.
        StressPass(const Device& device, const std::filesystem::path& shaderDirectory, double milliseconds);

        void record(VkCommandBuffer commands, GpuTimer& timer) const;

    private:
        ComputePipeline mPipeline;

        /// What the loop writes its answer to and nothing reads.
        Buffer mSink;

        std::uint32_t mIterations = 0;
    };
}
