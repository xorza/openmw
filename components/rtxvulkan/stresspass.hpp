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
    /// most on the frames a run measures first.
    class StressPass
    {
    public:
        /// @param milliseconds how long every frame's hold is to be.
        StressPass(const Device& device, const std::filesystem::path& shaderDirectory, double milliseconds);

        /// Records the hold into `commands`, timed as `RenderProfile::sHoldZone`.
        void record(VkCommandBuffer commands, GpuTimer& timer);

        /// What the loop's own clock said the last hold that finished came to, in nanoseconds:
        /// what was asked and the tick past it. Read once the submit that recorded the hold is
        /// waited for; a frame in flight is still writing it. The zone can only read longer — a
        /// clock switch stalls the card for a millisecond or so, and the loop's clock runs on
        /// through it — so this is the figure that says the loop did as it was told.
        std::uint32_t getHeldNs() const;

    private:
        ComputePipeline mPipeline;

        /// Where the loop leaves what its clock read, for `getHeldNs`.
        Buffer mSink;

        std::uint32_t mNanoseconds;
    };
}
