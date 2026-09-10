#include "framespan.hpp"

#include <format>
#include <utility>

#include <components/rtx/frameclock.hpp>

namespace MWRender
{
    std::optional<double> FrameSpan::enter(const std::chrono::steady_clock::time_point now)
    {
        const std::optional<double> since = mEnteredOnce ? std::optional(Rtx::since(mEntered, now)) : std::nullopt;

        mEntered = now;
        mEnteredOnce = true;

        return since;
    }

    double FrameSpan::sinceLeft(const std::chrono::steady_clock::time_point now) const
    {
        return Rtx::since(mLeft, now);
    }

    double FrameSpan::takePresent()
    {
        return std::exchange(mPresentMs, 0.0);
    }

    std::string_view SpeedReport::addFrame(const double frameMs)
    {
        if (!mRate.add(frameMs))
            return {};

        const auto written = std::format_to_n(mTitle.data(), mTitle.size() - 1, "OpenMW - {}", mRate.getText());
        *written.out = '\0';

        return std::string_view(mTitle.data(), static_cast<std::size_t>(written.out - mTitle.data()));
    }

    bool SpeedReport::addWait(const double waitMs)
    {
        mSpentMs += waitMs;
        ++mTimed;

        if (mTimed < sReportEvery)
            return false;

        mReported = std::exchange(mTimed, 0);
        return true;
    }
}
