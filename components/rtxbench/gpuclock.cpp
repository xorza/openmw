#include "gpuclock.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "benchspec.hpp"

namespace Rtx
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

    ClockWatch::~ClockWatch() = default;

    void ClockWatch::start()
    {
        // **Only where this call is what started it.** One of these is held across the places of a
        // suite, so a watch that kept what it saw would hand the second place the first place's
        // clock — and one that cleared a run already in progress would throw away the readings that
        // run had taken.
        if (!mWorker.start([this](std::stop_token stop) { watch(stop); }))
            return;

        const std::lock_guard<std::mutex> lock(mMutex);
        mSeen = GpuClock{};
    }

    GpuClock ClockWatch::stop()
    {
        // **One more before the join**, so the last frames are covered by a reading taken after
        // them rather than before, and a place short enough that the loop never came round still
        // answers with two.
        const GpuClock last = readGpuClock();

        mWorker.stop();

        const std::lock_guard<std::mutex> lock(mMutex);
        mSeen.add(last);

        return mSeen;
    }

    void ClockWatch::watch(std::stop_token stop)
    {
        // **Four a second.** Every reading forks this process, and a harness with a world loaded is
        // a large one to fork, so the rate is what the spawn cost was measured under rather than
        // what the card can be asked for.
        constexpr std::chrono::milliseconds sPeriod{ 250 };

        for (;;)
        {
            // Outside the lock: a spawn takes tens of milliseconds, and holding it for that would
            // make `stop` wait out a reading it is about to add its own to.
            const GpuClock now = readGpuClock();

            std::unique_lock<std::mutex> lock(mMutex);
            mSeen.add(now);

            // **Nothing ever notifies this**, and it is a condition variable so that the stop token
            // can break the wait: `std::this_thread::sleep_for` would hold the thread for the whole
            // period and make every join wait one out.
            if (mWake.wait_for(lock, stop, sPeriod, [&stop] { return stop.stop_requested(); }))
                return;
        }
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

        std::uint32_t core = 0;
        std::uint32_t memory = 0;
        std::uint32_t temperature = 0;
        std::uint64_t throttle = 0;
        if (!readNumber(fields[0], 10, core) || !readNumber(fields[1], 10, memory)
            || !readNumber(fields[2], 10, temperature) || !readNumber(fields[3], 16, throttle))
            return GpuClock{};

        return GpuClock::reading(core, memory, temperature, throttle);
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
