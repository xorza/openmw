#include "frametimer.hpp"

#include <cstddef>
#include <format>
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

    std::string_view FrameTimer::addFrame(const double frameMs)
    {
        if (!mRate.add(frameMs))
            return {};

        const auto written = std::format_to_n(mTitle.data(), mTitle.size() - 1, "OpenMW - {}", mRate.getText());
        *written.out = '\0';

        return std::string_view(mTitle.data(), static_cast<std::size_t>(written.out - mTitle.data()));
    }
}
