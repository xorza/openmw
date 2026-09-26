#include "gpuclock.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace RtxTool
{
    std::string describeThrottle(std::uint64_t mask)
    {
        // NVML's `nvmlClocksEventReason*` bits, in its own order. Idle is among them because a
        // reading taken from an idle card is a reading of the wrong thing, and saying so is the
        // whole point of quoting the clock at all.
        static constexpr std::array<std::pair<std::uint64_t, std::string_view>, 9> sReasons{ {
            { 0x0000000000000001ull, "gpu idle" },
            { 0x0000000000000002ull, "applications clocks setting" },
            { 0x0000000000000004ull, "sw power cap" },
            { 0x0000000000000008ull, "hw slowdown" },
            { 0x0000000000000010ull, "sync boost" },
            { 0x0000000000000020ull, "sw thermal slowdown" },
            { 0x0000000000000040ull, "hw thermal slowdown" },
            { 0x0000000000000080ull, "hw power brake" },
            { 0x0000000000000100ull, "display clock setting" },
        } };

        std::string named;
        for (const auto& [bit, name] : sReasons)
        {
            if ((mask & bit) == 0)
                continue;

            if (!named.empty())
                named += ", ";

            named += name;
        }

        // A bit this does not know is still worth saying: a driver that grew a reason should read as
        // an unknown one rather than as a card nothing is holding back.
        std::uint64_t known = 0;
        for (const auto& [bit, name] : sReasons)
            known |= bit;

        if ((mask & ~known) != 0)
        {
            if (!named.empty())
                named += ", ";

            named += std::format("unknown reason {:#018x}", mask & ~known);
        }

        return named;
    }

    void GpuClock::add(const GpuClock& other)
    {
        if (!other.mRead)
            return;

        if (!mRead)
        {
            *this = other;
            return;
        }

        mLowestMhz = std::min(mLowestMhz, other.mLowestMhz);
        mHighestMhz = std::max(mHighestMhz, other.mHighestMhz);
        mSumMhz += other.mSumMhz;
        mReadings += other.mReadings;
        mMemoryMhz = std::max(mMemoryMhz, other.mMemoryMhz);
        mTemperatureC = std::max(mTemperatureC, other.mTemperatureC);
        mThrottleMask |= other.mThrottleMask;
    }

    GpuClock GpuClock::reading(const std::uint32_t coreMhz, const std::uint32_t memoryMhz,
        const std::uint32_t temperatureC, const std::uint64_t throttle)
    {
        return GpuClock{
            .mLowestMhz = coreMhz,
            .mHighestMhz = coreMhz,
            .mSumMhz = coreMhz,
            .mReadings = 1,
            .mMemoryMhz = memoryMhz,
            .mTemperatureC = temperatureC,
            .mThrottleMask = throttle,
            .mRead = true,
        };
    }

    std::string describeClock(const GpuClock& clock)
    {
        if (!clock.mRead)
            return {};

        // **The mean first, because it is the number a frame time is read against**, with the ends
        // beside it saying whether the card moved while the frames were drawn. How many readings
        // made them is what tells a range worth reading from two samples that happened to agree.
        // **The count is printed whether or not the clock moved.** A card that held still over
        // twenty-nine readings and one asked once say the same number otherwise, and only the first
        // of them is a reading to hold a frame time against.
        const std::string spread = clock.mLowestMhz == clock.mHighestMhz
            ? std::string()
            : std::format(", {}–{}", clock.mLowestMhz, clock.mHighestMhz);

        const std::string core = std::format("{} MHz core over {} reading{}{}", clock.getMeanMhz(), clock.mReadings,
            clock.mReadings == 1 ? "" : "s", spread);

        const std::string throttle = describeThrottle(clock.mThrottleMask);

        return std::format("  clock {}, {} MHz memory, {} °C — {}\n", core, clock.mMemoryMhz, clock.mTemperatureC,
            throttle.empty() ? "nothing holding it back" : throttle);
    }
}
