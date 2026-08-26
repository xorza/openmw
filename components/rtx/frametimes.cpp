#include "frametimes.hpp"

#include <format>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <string_view>

namespace Rtx
{
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
    void GpuBreakdown::add(std::span<const GpuSpan> spans)
    {
        for (const GpuSpan& span : spans)
        {
            // The index and not the iterator: adding a name invalidates whatever `find` returned,
            // and the row about to be pushed to is the one that name is at.
            const auto at
                = static_cast<std::size_t>(std::find(mNames.begin(), mNames.end(), span.mName) - mNames.begin());

            if (at == mNames.size())
            {
                mNames.emplace_back(span.mName);
                mTimes.emplace_back();
            }

            mTimes[at].push_back(span.mMs);
        }
    }

    std::span<const GpuZone> GpuBreakdown::summariseZones()
    {
        mZones.clear();
        mZones.reserve(mNames.size());

        for (std::size_t at = 0; at < mNames.size(); ++at)
            mZones.push_back(GpuZone{ .mName = mNames[at], .mTimes = summarise(mTimes[at]) });

        std::sort(mZones.begin(), mZones.end(),
            [](const GpuZone& a, const GpuZone& b) { return a.mTimes.mMedian > b.mTimes.mMedian; });

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

    std::string describeZones(std::span<const GpuZone> zones)
    {
        if (zones.empty())
            return {};

        std::string row = "  gpu ms  ";
        for (const GpuZone& zone : zones)
            row += std::format("  {} {:.2f}", zone.mName, zone.mTimes.mMedian);

        return row + "\n";
    }
}
