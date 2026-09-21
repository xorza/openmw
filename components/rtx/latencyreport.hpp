#pragma once

#include <cstdint>

namespace Rtx
{
    /// What the driver measured of one finished frame: the one figure a player feels, from the
    /// markers the frame set. In the core because a report is not a Vulkan fact, and the backend
    /// fills it from whatever its API keeps. The driver keeps more — the simulation's, the
    /// submission's and the device's own stretches — and a reader for those is what would bring
    /// them here.
    struct LatencyReport
    {
        /// Which present the frame ended in, as the backend numbers them: what tells this frame's
        /// report from the last frame's.
        std::uint64_t mPresentId = 0;

        /// From the frame's input sample to the end of its present call, in microseconds: the
        /// host's part of the latency, which the sleep is there to shorten.
        std::uint64_t mInputToPresentUs = 0;
    };
}
