#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Rtx
{
    /// How fast a measured run steps the world, in frames a second.
    ///
    /// **World time and not wall time, which is the whole of what makes two runs comparable.** The
    /// world steps a sixtieth of a second per frame however long the frame took, so ten seconds is
    /// six hundred frames — the same six hundred frames, with the same particles in the same places
    /// and the same sample in each pixel, on a build that draws them in four seconds and on one
    /// that takes twenty. A run against the clock would animate further on the faster build and
    /// measure a different scene.
    ///
    /// Sixty because that is what the frame budget is written against, and because an actor has to
    /// move the same amount per frame here as it does in a played session.
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

    /// The whole of what a run's length is written as, in one spelling for both hosts.
    ///
    ///     240             measure 240 frames
    ///     10s             measure ten seconds of world instead
    ///     10s:2s          the same, after two seconds warming up
    ///     10s:2s@12000    and fly forwards at 12000 units a second while measuring
    ///
    /// **One parser, because the two hosts used to have two.** The harness took `--seconds`,
    /// `--warmup`, `--frames` and a route's speed as four options and the game took this string, so
    /// a run asked for in one could not be repeated in the other without translating it by hand.
    struct BenchSpec
    {
        BenchSpan mRun;
        BenchSpan mWarm;

        /// World units a second, or zero for a run that stands still.
        float mSpeed = 0.0f;

        std::uint32_t getMeasured() const { return mRun.getFrames(); }
        std::uint32_t getWarmup() const { return mWarm.getFrames(); }
    };

    /// Reads the whole spelling. Nothing, with the reason in `complaint`, where it will not parse.
    ///
    /// **A run nobody can read the settings of is not a run.** A spec that will not parse is a typo
    /// in a benchmark somebody is about to trust, and starting anyway would hand them a number for
    /// a length they did not ask for.
    std::optional<BenchSpec> readSpec(std::string_view text, std::string& complaint);

    /// Splits a comma-separated list, dropping the space around each name and any empty entry.
    ///
    /// Shared by every list this fork writes down — the views a suite names, `--views`, and the
    /// fields of the one csv line the GPU clock is read from — so a list written on a command line
    /// and a list written in a file cannot come to disagree about a trailing comma.
    std::vector<std::string> splitNames(std::string_view text);
}
