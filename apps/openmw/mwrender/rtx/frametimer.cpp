#include "frametimer.hpp"

#include <cstddef>
#include <format>
#include <optional>
#include <utility>

#include <components/rtx/framespend.hpp>

namespace MWRender
{
    std::optional<double> FrameTimer::enter(const std::chrono::steady_clock::time_point now)
    {
        const std::optional<double> since
            = mEntered.has_value() ? std::optional(Rtx::since(*mEntered, now)) : std::nullopt;
        mEntered = now;
        return since;
    }

    double FrameTimer::sinceLeft(const std::chrono::steady_clock::time_point now) const
    {
        return Rtx::since(mLeft, now);
    }

    double FrameTimer::takePresent()
    {
        return std::exchange(mPresentMs, 0.0);
    }

    std::string_view FrameTimer::addFrame(const double frameMs, const std::optional<Rtx::LatencyReport>& latency)
    {
        if (!mRate.add(frameMs))
            return {};

        // The newest frame's and not the second's, because the second is the rate's: a latency
        // averaged over a second would hide the frame the sleep let slip.
        const auto written = latency.has_value()
            ? std::format_to_n(mTitle.data(), mTitle.size() - 1, "OpenMW - {}, {:.1f} ms latency", mRate.getText(),
                  static_cast<double>(latency->mInputToPresentUs) / 1000.0)
            : std::format_to_n(mTitle.data(), mTitle.size() - 1, "OpenMW - {}", mRate.getText());
        *written.out = '\0';

        return std::string_view(mTitle.data(), static_cast<std::size_t>(written.out - mTitle.data()));
    }
}
