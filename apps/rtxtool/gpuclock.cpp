#include "gpuclock.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "benchsuite.hpp"

namespace RtxTool
{
    namespace
    {
        /// Everything `nvidia-smi` wrote, or empty where it could not be run.
        ///
        /// stderr is discarded: a machine without the tool, or with a driver that refuses the query,
        /// is a machine this reports no clock for rather than one that prints a shell error into the
        /// middle of a report.
        std::string ask()
        {
            std::FILE* pipe = popen(
                "nvidia-smi --query-gpu=clocks.gr,clocks.mem,temperature.gpu,"
                "clocks_event_reasons.active --format=csv,noheader,nounits 2>/dev/null",
                "r");
            if (pipe == nullptr)
                return {};

            std::string answer;
            std::array<char, 256> buffer{};
            while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr)
                answer += buffer.data();

            // **The status is not read, only what was written.** A tool that could not answer
            // prints nothing here — its complaint goes to the stderr discarded above — so the
            // reading itself is the test, and a status this cannot always get (a host that reaps
            // its own children answers -1) cannot throw a good one away.
            pclose(pipe);
            return answer;
        }

        /// `text` as a number in `base`, or nothing where it is not one — which is what `[N/A]` is,
        /// and what a laptop's card answers for a field its driver does not expose.
        template <class T>
        bool readNumber(std::string_view text, int base, T& into)
        {
            if (text.starts_with("0x") || text.starts_with("0X"))
                text.remove_prefix(2);

            const char* end = text.data() + text.size();
            const std::from_chars_result read = std::from_chars(text.data(), end, into, base);
            return read.ec == std::errc{} && read.ptr == end;
        }
    }

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
        mMemoryMhz = std::max(mMemoryMhz, other.mMemoryMhz);
        mTemperatureC = std::max(mTemperatureC, other.mTemperatureC);
        mThrottleMask |= other.mThrottleMask;
    }

    GpuClock readGpuClock()
    {
        const std::string answer = ask();

        // **The list splitter the view file and `--views` are read by**, over the one line of csv
        // this asked for. It drops an empty entry, which a positional read would normally mind: here
        // a dropped column takes the count under four and the whole reading is refused, so a field
        // the driver could not fill can never be read as the field beside it.
        const std::vector<std::string> fields = splitNames(std::string_view(answer).substr(0, answer.find('\n')));
        if (fields.size() < 4)
            return GpuClock{};

        GpuClock clock;
        if (!readNumber(fields[0], 10, clock.mLowestMhz) || !readNumber(fields[1], 10, clock.mMemoryMhz)
            || !readNumber(fields[2], 10, clock.mTemperatureC) || !readNumber(fields[3], 16, clock.mThrottleMask))
            return GpuClock{};

        clock.mHighestMhz = clock.mLowestMhz;
        clock.mRead = true;
        return clock;
    }

    std::string describeClock(const GpuClock& clock)
    {
        if (!clock.mRead)
            return {};

        const std::string core = clock.mLowestMhz == clock.mHighestMhz
            ? std::format("{} MHz", clock.mLowestMhz)
            : std::format("{}–{} MHz", clock.mLowestMhz, clock.mHighestMhz);

        const std::string throttle = describeThrottle(clock.mThrottleMask);

        return std::format("  clock {} core, {} MHz memory, {} °C — {}\n", core, clock.mMemoryMhz, clock.mTemperatureC,
            throttle.empty() ? "nothing holding it back" : throttle);
    }
}
