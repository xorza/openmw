#pragma once

#include <array>
#include <cstddef>
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
    /// **Held by measuring the card on every frame, and never by a count taken once.** How long
    /// a count of iterations takes is the card's clock, and the clock is whatever the card is at:
    /// a count read off a hundred milliseconds of dispatches at start held 1.4 ms of the 8 asked,
    /// because the card was still at the clock it idles at and a dispatch with a wait between
    /// each did not bring it up — and it read 2 ms after a five-second compile against 8 the run
    /// after. So every frame's reading of the zone, over the count that frame ran, is the card's
    /// rate then, and the next frame's count is the asked time at that rate. The rate and not the
    /// shortfall: a reading comes back two or three frames after the count it measured, and a
    /// count corrected by the shortfall of a stale reading swung between four and sixteen
    /// milliseconds six frames apart and never settled.
    class StressPass
    {
    public:
        /// @param milliseconds how long every frame's hold is to be.
        StressPass(const Device& device, const std::filesystem::path& shaderDirectory, double milliseconds);

        /// Records the hold into frame `frame`'s commands, and remembers the count it ran for
        /// `follow`.
        void record(VkCommandBuffer commands, GpuTimer& timer, std::uint64_t frame);

        /// Takes what frame `frame`'s hold measured, in milliseconds, as the card's rate over the
        /// count that frame ran, and sets the next count from it.
        void follow(std::uint64_t frame, double heldMs);

    private:
        /// How many frames' counts are kept, by frame number: more than are ever in flight or
        /// waiting to be collected.
        static constexpr std::size_t sRemembered = 8;

        ComputePipeline mPipeline;

        /// What the loop writes its answer to and nothing reads.
        Buffer mSink;

        double mAskedMs;

        /// Where the count starts: about a millisecond's worth at the clock this card runs at,
        /// which the first frame back replaces.
        std::uint32_t mIterations = 1u << 18;

        /// The card's rate as the frames have read it, in milliseconds an iteration; nought
        /// before the first frame back.
        double mMsPerIteration = 0.0;

        /// The count each recent frame ran, so its reading can be read as a rate.
        std::array<std::uint32_t, sRemembered> mCounts{};
    };
}
