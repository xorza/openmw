#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace RtxTool
{
    /// How much of a run a number asks for: frames, or seconds where it names those.
    struct BenchSpan
    {
        /// How long a window runs when nobody said: until it is closed. A count rather than a
        /// special case, so one schedule serves a run of eight frames and a session somebody flies
        /// for an hour — and named, so that what makes room for a run's frames can tell the two
        /// apart: room for this many is thirty gigabytes a row, which Linux only promises and
        /// Windows commits.
        static constexpr std::uint32_t sUntilClosed = ~0u;

        std::uint32_t mFrames = 0;
        float mSeconds = 0.0f;

        bool empty() const { return mFrames == 0 && mSeconds <= 0.0f; }
        bool isUntilClosed() const { return mFrames == sUntilClosed; }

        /// How many frames this comes to at `step` seconds a frame, the run's own
        /// (`RunSetup::getWorldStep`). At least one for a span that asked for anything at all, and
        /// nought for one that asked for nothing.
        std::uint32_t getFrames(float step) const;
    };

    /// How long a stop runs and how much of it is thrown away first. Filled from the command line
    /// by the harness; a route's speed is the route's own (`Route::mSpeed`).
    struct BenchSpec
    {
        BenchSpan mRun;
        BenchSpan mWarm;

        std::uint32_t getMeasured(float step) const { return mRun.getFrames(step); }
        std::uint32_t getWarmup(float step) const { return mWarm.getFrames(step); }
    };

    /// Splits a comma-separated list, dropping the space around each name and any empty entry. One
    /// splitter for every list this fork writes down, so a command line and a file cannot disagree
    /// about a trailing comma.
    std::vector<std::string> splitNames(std::string_view text);
}
