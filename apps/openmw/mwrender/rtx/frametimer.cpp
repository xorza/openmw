#include "frametimer.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <utility>

#include <components/rtx/framespend.hpp>

namespace MWRender
{
    namespace
    {
        /// How much frame time closes a second of the title. A second, so the figure moves as often
        /// as a clock's.
        constexpr double sSecondMs = 1000.0;
    }

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
        mSummedMs += frameMs;
        mWorstMs = std::max(mWorstMs, frameMs);
        ++mFrames;

        if (mSummedMs < sSecondMs)
            return false;

        mSecondMeanMs = mSummedMs / mFrames;
        mSecondWorstMs = mWorstMs;

        mSummedMs = 0.0;
        mWorstMs = 0.0;
        mFrames = 0;

        return true;
    }

    std::string_view FrameTimer::writeTitle(
        const std::optional<Rtx::LatencyReport>& latency, const std::string_view note)
    {
        char* out = mTitle.data();
        const auto room = [&] { return static_cast<std::size_t>(mTitle.data() + mTitle.size() - 1 - out); };

        out = std::format_to_n(out, room(), "OpenMW").out;
        if (mSecondMeanMs > 0.0)
            out = std::format_to_n(out, room(), " - {:.0f} fps, {:.1f} ms, worst {:.1f} ms", sSecondMs / mSecondMeanMs,
                mSecondMeanMs, mSecondWorstMs)
                      .out;

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
