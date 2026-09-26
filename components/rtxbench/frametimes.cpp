#include "frametimes.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <components/files/conversion.hpp>
#include <components/platform/fifo.hpp>

namespace Rtx
{
    namespace
    {
        /// How much frame time closes a line. A second, so the figure moves as often as a clock's.
        constexpr double sLineMs = 1000.0;
    }

    bool FrameRate::add(const double frameMs)
    {
        mSummedMs += frameMs;
        mWorstMs = std::max(mWorstMs, frameMs);
        ++mFrames;

        if (mSummedMs < sLineMs)
            return false;

        const double meanMs = mSummedMs / mFrames;
        const auto [end, length] = std::format_to_n(
            mText.data(), mText.size(), "{:.0f} fps, {:.1f} ms, worst {:.1f} ms", sLineMs / meanMs, meanMs, mWorstMs);

        // A line is under forty characters at any rate a frame can have, so the buffer is not a
        // limit anything reaches — but a truncated line is still a line, and not an overrun.
        assert(static_cast<std::size_t>(length) <= mText.size() && "the frame rate line outgrew its buffer");
        mLength = std::min(static_cast<std::size_t>(length), mText.size());

        mSummedMs = 0.0;
        mWorstMs = 0.0;
        mFrames = 0;

        return true;
    }

    PerfControl::PerfControl(std::filesystem::path fifo)
        : mFifo(std::move(fifo))
    {
    }

    void PerfControl::enable()
    {
        if (mFifo.empty())
            return;

        if (mHandle == Platform::File::Handle::Invalid)
            mHandle = Platform::Fifo::openForWriting(mFifo);

        send("enable\n");
    }

    void PerfControl::disable()
    {
        if (mHandle == Platform::File::Handle::Invalid)
            return;

        send("disable\n");
    }

    void PerfControl::send(std::string_view command)
    {
        Platform::Fifo::write(mHandle, command.data(), command.size());
    }

    namespace
    {
        /// The `quantile`th value of an already sorted `times`, by nearest rank.
        double rank(const std::vector<double>& times, double quantile)
        {
            const std::size_t place = static_cast<std::size_t>(std::ceil(quantile * static_cast<double>(times.size())));

            // `ceil` of a positive quantile is at least one, and the clamp above catches the other
            // end: a p99 of a run of ten is the tenth of ten rather than the eleventh.
            return times[std::min(times.size(), std::max<std::size_t>(place, 1)) - 1];
        }
    }

    double FrameTimes::getRate() const
    {
        return mMedian > 0.0 ? 1000.0 / mMedian : 0.0;
    }

    double FrameTimes::getLowRate() const
    {
        return mP99 > 0.0 ? 1000.0 / mP99 : 0.0;
    }

    void writeFrameTimes(const std::filesystem::path& path, const FrameSamples& samples)
    {
        std::ofstream file(path);
        if (!file)
            throw std::runtime_error("could not open " + Files::pathToUnicodeString(path));

        for (const Timing timing : sTimings.values())
            file << (indexOf(timing) == 0 ? "" : " ") << sTimings.name(timing);
        file << '\n';

        for (std::uint32_t frame = 0; frame < samples.size(); ++frame)
        {
            for (const Timing timing : sTimings.values())
                file << (indexOf(timing) == 0 ? "" : " ") << std::format("{:.3f}", samples.at(timing)[frame]);
            file << '\n';
        }
    }

    FrameTimes summarise(std::vector<double>& times)
    {
        assert(!times.empty() && "a run with no frames in it has no times to summarise");

        std::sort(times.begin(), times.end());

        return FrameTimes{
            .mMean = std::accumulate(times.begin(), times.end(), 0.0) / static_cast<double>(times.size()),
            .mMedian = rank(times, 0.5),
            .mP95 = rank(times, 0.95),
            .mP99 = rank(times, 0.99),
            .mBest = times.front(),
            .mWorst = times.back(),
        };
    }
}

namespace Rtx
{
    void GpuBreakdown::reserve(const std::uint32_t frames)
    {
        mReserved = frames;
        for (ZoneRow& row : mRows)
            row.mTimes.reserve(frames);
    }

    void GpuBreakdown::clear()
    {
        for (ZoneRow& row : mRows)
        {
            row.mTimes.clear();
            row.mSeen = 0;
        }

        mFrames = 0;
        mZones.clear();
    }

    void GpuBreakdown::add(std::span<const GpuSpan> spans)
    {
        ++mFrames;

        for (const GpuSpan& span : spans)
        {
            // The index and not the iterator: adding a row invalidates whatever `find_if` returned,
            // and the row about to be pushed to is the one that name is at.
            const auto at = static_cast<std::size_t>(std::find_if(mRows.begin(), mRows.end(), [&](const ZoneRow& row) {
                return row.mName == span.mName;
            }) - mRows.begin());

            if (at == mRows.size())
            {
                mRows.push_back(ZoneRow{ .mName = span.mName });

                // **Room for the run taken on the frame the zone first appears.** A row that grows
                // does it inside a frame it is timing, and what a growth costs is a copy of every
                // sample taken so far — landing on one frame of the run and reported as its worst.
                mRows.back().mTimes.reserve(mReserved);
            }

            ZoneRow& row = mRows[at];

            // **One sample a frame, whatever a frame opened the zone.** A pass recorded in
            // batches — the structure builds are — opens its zone several times over one frame, and
            // a row longer than the run then reported a zone as running on more frames than there
            // were: `tlas 0.24 on 620 of 601`. What a frame's budget is spent on is what the frame
            // spent there, so the spans of one frame are that frame's sample.
            if (row.mSeen == mFrames)
                row.mTimes.back() += span.mMs;
            else
            {
                row.mTimes.push_back(span.mMs);
                row.mSeen = mFrames;
            }
        }
    }

    std::span<const GpuZone> GpuBreakdown::summariseZones()
    {
        mZones.clear();
        mZones.reserve(mRows.size());

        for (ZoneRow& row : mRows)
        {
            // A zone an earlier stop met and this one did not.
            if (row.mTimes.empty())
                continue;

            const double spent = std::accumulate(row.mTimes.begin(), row.mTimes.end(), 0.0);

            mZones.push_back(GpuZone{
                .mName = row.mName,
                .mTimes = summarise(row.mTimes),
                .mFrames = static_cast<std::uint32_t>(row.mTimes.size()),
                .mOfFrames = mFrames,
                .mShareMs = spent / static_cast<double>(mFrames),
            });
        }

        // **By what each cost the run and not by what it cost a frame that ran it**, which is the
        // order "where did the frame go" is asked in: a pass that runs at a cell crossing is
        // seven milliseconds and a fifth of a per-cent of the run, and sorting it to the top of
        // the row put it above the frame median printed over it.
        std::sort(
            mZones.begin(), mZones.end(), [](const GpuZone& a, const GpuZone& b) { return a.mShareMs > b.mShareMs; });

        return mZones;
    }

    std::string describeHeadings()
    {
        return std::format(
            "  {:<9}{:>9}{:>10}{:>10}{:>10}{:>10}{:>10}\n", "", "median", "mean", "p95", "p99", "best", "worst");
    }

    std::string describeTimes(std::string_view heading, const FrameTimes& times)
    {
        return std::format("  {:<9}{:9.2f}{:10.2f}{:10.2f}{:10.2f}{:10.2f}{:10.2f}\n", heading, times.mMedian,
            times.mMean, times.mP95, times.mP99, times.mBest, times.mWorst);
    }

    std::string describeZone(const GpuZone& zone)
    {
        if (zone.isEveryFrame())
            return std::format("{} {:.2f}", zone.mName, zone.mShareMs);

        // What it cost when it ran, and how rarely — the two figures the share is the product of,
        // and without them a pass that stalls a frame every sixty of them reads as a rounding
        // error.
        return std::format("{} {:.2f} ({:.2f} on {} of {})", zone.mName, zone.mShareMs, zone.mTimes.mMedian,
            zone.mFrames, zone.mOfFrames);
    }

    std::string describeZones(std::span<const GpuZone> zones)
    {
        if (zones.empty())
            return {};

        std::string row = "  gpu ms  ";
        for (const GpuZone& zone : zones)
            row += "  " + describeZone(zone);

        return row + "\n";
    }
}
