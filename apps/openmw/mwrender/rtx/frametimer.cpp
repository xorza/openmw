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

    bool FrameTimer::addFrame(const double frameMs)
    {
        return mRate.add(frameMs);
    }

    std::string_view FrameTimer::writeTitle(
        const std::optional<Rtx::LatencyReport>& latency, const std::string_view note)
    {
        char* out = mTitle.data();
        const auto room = [&] { return static_cast<std::size_t>(mTitle.data() + mTitle.size() - 1 - out); };

        out = std::format_to_n(out, room(), "OpenMW - {}", mRate.getText()).out;

        // The newest frame's and not the second's, because the second is the rate's: a latency
        // averaged over a second would hide the frame the sleep let slip.
        if (latency.has_value())
            out = std::format_to_n(
                out, room(), ", {:.1f} ms latency", static_cast<double>(latency->mInputToPresentUs) / 1000.0)
                      .out;

        if (!note.empty())
            out = std::format_to_n(out, room(), " - {}", note).out;

        *out = '\0';
        return std::string_view(mTitle.data(), static_cast<std::size_t>(out - mTitle.data()));
    }
}
