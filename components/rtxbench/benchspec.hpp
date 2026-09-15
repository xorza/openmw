#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Rtx
{
    /// How fast a measured run steps the world, in frames a second: world time and not wall time,
    /// so ten seconds is the same six hundred frames on a build that draws them in four seconds and
    /// on one that takes twenty. Sixty because that is what the frame budget is written against.
    inline constexpr float sStepRate = 60.0f;

    /// How long one of those frames stands for, which is what a measured frame tells the renderer.
    inline constexpr float sStepSeconds = 1.0f / sStepRate;

    /// How much of a run a number asks for: frames, or seconds where it names those.
    struct BenchSpan
    {
        std::uint32_t mFrames = 0;
        float mSeconds = 0.0f;

        bool empty() const { return mFrames == 0 && mSeconds <= 0.0f; }

        /// How many frames this comes to at the rate the world steps. At least one for a span that
        /// asked for anything at all, and nought for one that asked for nothing.
        std::uint32_t getFrames() const;
    };

    /// How long a stop runs and how much of it is thrown away first. Filled from the command line
    /// by the harness; a route's speed is the route's own (`Route::mSpeed`).
    struct BenchSpec
    {
        BenchSpan mRun;
        BenchSpan mWarm;

        std::uint32_t getMeasured() const { return mRun.getFrames(); }
        std::uint32_t getWarmup() const { return mWarm.getFrames(); }
    };

    /// Splits a comma-separated list, dropping the space around each name and any empty entry. One
    /// splitter for every list this fork writes down, so a command line and a file cannot disagree
    /// about a trailing comma.
    std::vector<std::string> splitNames(std::string_view text);
}
