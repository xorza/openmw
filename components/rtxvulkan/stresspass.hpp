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
    /// `RenderProfile::mStressOverlapMs`. Appended to every frame's trace, it keeps the device
    /// that far behind the host, so every frame is recorded over a frame still running: a hazard
    /// that needs the overlap to show shows on the first frame of every run rather than on one run
    /// in four. The zone it is timed as is `RenderProfile::sHoldZone`, so the report shows what it
    /// actually held.
    ///
    /// **The time is measured where it passes, in the loop, off the device's real-time clock.**
    /// `stress.comp` says why a count is not a time on a card whose clock moves, and it moves the
    /// most on the frames a run measures first. What the loop's clock came to is left in the
    /// frame's counts, `Shaders::FrameCounts::mHeldNs`, which the ring reads back once the frame is
    /// waited for — `FrameResult::mHeldMs`.
    class StressPass
    {
    public:
        /// @param milliseconds how long every frame's hold is to be.
        StressPass(const Device& device, const std::filesystem::path& shaderDirectory, double milliseconds);

        /// Records the hold into `commands`, timed as `RenderProfile::sHoldZone`, leaving what the
        /// loop's clock read in `counts`: the frame's own block, so the reading is the frame's and
        /// not whichever frame in flight wrote last.
        void record(VkCommandBuffer commands, GpuTimer& timer, const Buffer& counts);

    private:
        ComputePipeline mPipeline;

        std::uint32_t mNanoseconds;
    };
}
