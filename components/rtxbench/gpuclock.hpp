#pragma once

#include <cstdint>
#include <string>

namespace Rtx
{
    /// What the device's clock and power state were, as `nvidia-smi` reports them.
    ///
    /// **A frame time without its clock is not a number to compare.** A card under load is held at
    /// whatever its power budget allows: this one runs about 1.8 GHz there against 2.3 GHz cool, so
    /// the same build measures several per cent apart from one run to the next. A run that says
    /// which clock it was taken at can be held against a run taken on another day.
    ///
    /// **A range and not a reading, because a card moves while a place is measured.** One sample is
    /// one moment, and a moment sampled after the frames stopped is a card already climbing back —
    /// which reads as a fast clock over frames drawn at a slower one. What the two ends of a place
    /// say bounds the frames between them.
    struct GpuClock
    {
        /// The graphics clock over every reading taken. Equal where only one was.
        std::uint32_t mLowestMhz = 0;
        std::uint32_t mHighestMhz = 0;

        std::uint32_t mMemoryMhz = 0;

        /// The highest any reading saw, which is the one that explains a throttle.
        std::uint32_t mTemperatureC = 0;

        /// Why the card was not running faster, as NVML's own bits, or-ed over every reading.
        /// `describeThrottle` turns it into words.
        std::uint64_t mThrottleMask = 0;

        /// False where nothing answered — no `nvidia-smi`, another vendor's device, or an answer
        /// this cannot read. Such a run reports no clock rather than a made-up one.
        bool mRead = false;

        /// Takes `other` in: the clock spans both, and the reasons are what either saw. A reading
        /// that answered nothing adds nothing, so a machine that answers once and then not again
        /// still reports the once.
        void add(const GpuClock& other);
    };

    /// Asks the device what it is doing now, as one reading.
    ///
    /// **A process spawn, so it is asked at the ends of a place and never on a frame path.** A frame
    /// that waited for `nvidia-smi` would be the worst frame of the run and would say so in the p99.
    GpuClock readGpuClock();

    /// The clock as one line of the report, or empty where nothing answered.
    std::string describeClock(const GpuClock& clock);

    /// What a throttle mask names, in the order NVML's bits are numbered, or empty for a card that
    /// nothing is holding back.
    std::string describeThrottle(std::uint64_t mask);
}
