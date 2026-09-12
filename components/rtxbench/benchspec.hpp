#pragma once

#include <cstdint>
#include <optional>
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

    /// How much of a run a number asks for: frames, or seconds where it carries an `s`.
    struct BenchSpan
    {
        std::uint32_t mFrames = 0;
        float mSeconds = 0.0f;

        bool empty() const { return mFrames == 0 && mSeconds <= 0.0f; }

        /// How many frames this comes to at the rate the world steps. At least one for a span that
        /// asked for anything at all, and nought for one that asked for nothing.
        std::uint32_t getFrames() const;

        /// The span as a report spells it: `600 frames` or `10 s`.
        std::string describe() const;
    };

    /// `240` frames, or `10s` seconds. Nothing where it is neither.
    std::optional<BenchSpan> readSpan(std::string_view text);

    /// The whole of what a run's length is written as, in one spelling for both hosts so a run asked
    /// for in one can be repeated in the other: `240` frames, `10s` seconds of world, `10s:2s` after
    /// two seconds warming up, `10s:2s@12000` flying forwards at 12000 units a second meanwhile.
    struct BenchSpec
    {
        BenchSpan mRun;
        BenchSpan mWarm;

        /// World units a second, or zero for a run that stands still.
        float mSpeed = 0.0f;

        std::uint32_t getMeasured() const { return mRun.getFrames(); }
        std::uint32_t getWarmup() const { return mWarm.getFrames(); }
    };

    /// Reads the whole spelling. Nothing, with the reason in `complaint`, where it will not parse:
    /// starting anyway would hand somebody a number for a length they did not ask for.
    std::optional<BenchSpec> readSpec(std::string_view text, std::string& complaint);

    /// Splits a comma-separated list, dropping the space around each name and any empty entry. One
    /// splitter for every list this fork writes down, so a command line and a file cannot disagree
    /// about a trailing comma.
    std::vector<std::string> splitNames(std::string_view text);
}
