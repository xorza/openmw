#pragma once

#include <cstdint>
#include <string>

namespace Rtx
{
    /// The card's clock over a place's frames. A frame time without its clock is not a number to
    /// compare: under load this card is held near 1.8 GHz against 2.3 GHz cool, and the same build
    /// measures several per cent apart from one run to the next. A range and not a reading, because
    /// a card moves while a place is measured, and `CardWatch` samples through the frames because
    /// two ends cannot say what the clock did between them.
    struct GpuClock
    {
        /// The graphics clock over every reading taken. Equal where only one was.
        std::uint32_t mLowestMhz = 0;
        std::uint32_t mHighestMhz = 0;

        /// Every reading's core clock summed, and how many there were: the mean is what a frame
        /// time is read against, and a low that one reading of a hundred saw is noise.
        std::uint64_t mSumMhz = 0;
        std::uint32_t mReadings = 0;

        std::uint32_t mMemoryMhz = 0;

        /// The highest any reading saw, which is the one that explains a throttle.
        std::uint32_t mTemperatureC = 0;

        /// Why the card was not running faster, as NVML's own bits, or-ed over every reading.
        std::uint64_t mThrottleMask = 0;

        /// False where nothing answered — no driver library to ask, another vendor's device — so
        /// such a run reports no clock rather than a made-up one.
        bool mRead = false;

        /// Takes `other` in: the clock spans both, and the reasons are what either saw. A reading
        /// that answered nothing adds nothing.
        void add(const GpuClock& other);

        /// One reading, with the range, the sum and the count that one reading implies. Named,
        /// because a caller that set four of the five fields would report a mean of nought.
        static GpuClock reading(
            std::uint32_t coreMhz, std::uint32_t memoryMhz, std::uint32_t temperatureC, std::uint64_t throttle);

        /// The mean core clock, or nought where nothing answered.
        std::uint32_t getMeanMhz() const { return mReadings > 0 ? static_cast<std::uint32_t>(mSumMhz / mReadings) : 0; }
    };

    /// The clock as one line of the report, or empty where nothing answered.
    std::string describeClock(const GpuClock& clock);

    /// What a throttle mask names, in the order NVML's bits are numbered, or empty for a card that
    /// nothing is holding back.
    std::string describeThrottle(std::uint64_t mask);
}
