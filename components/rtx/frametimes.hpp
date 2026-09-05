#pragma once

#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "renderer.hpp"

namespace Rtx
{
    /// Milliseconds between two readings of the steady clock, which is what every figure here is.
    ///
    /// **One spelling, because it was written seven times.** `std::chrono::duration<double,
    /// std::milli>(to - from).count()` says nothing a reader needs and hides which way round the
    /// subtraction goes; every timed stretch in this fork now reads the same way.
    inline double since(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to)
    {
        return std::chrono::duration<double, std::milli>(to - from).count();
    }

    /// What a run of frame times came to, in milliseconds.
    ///
    /// **A distribution and not an average**, because the two questions a renderer gets asked are
    /// different ones. "Is this build faster than that one" is answered by the middle of the run;
    /// "does this place play badly" is answered by its tail, and a mean hides exactly the frames
    /// that make a picture stutter. Both are here so neither has to be inferred from the other.
    struct FrameTimes
    {
        double mMean = 0.0;
        double mMedian = 0.0;

        /// The frame that only one in twenty, and one in a hundred, are worse than.
        double mP95 = 0.0;
        double mP99 = 0.0;

        double mBest = 0.0;
        double mWorst = 0.0;

        /// Frames a second, were every frame the median one.
        double getRate() const;

        /// Frames a second at the ninety-ninth percentile — the "one per cent low" a frame rate is
        /// usually quoted with, and the number that says whether a run was smooth.
        double getLowRate() const;
    };

    /// The four figures a measured frame contributes, gathered over one run.
    ///
    /// **One object because they are cleared, filled and summarised together.** Four vectors kept
    /// apart are four chances for a frame to reach three of them, and rows out of step with each
    /// other are rows that cannot be read against each other at all.
    ///
    /// **Shared by the harness and the game**, whose two reports only mean something beside each
    /// other: a crossing in one is measured against a crossing in the other, and a row one of them
    /// gathered differently would be a difference read as a finding.
    struct FrameSamples
    {
        std::vector<double> mFrame;
        std::vector<double> mWait;
        std::vector<double> mWalk;
        std::vector<double> mPlace;

        void reserve(std::uint32_t frames)
        {
            mFrame.reserve(frames);
            mWait.reserve(frames);
            mWalk.reserve(frames);
            mPlace.reserve(frames);
        }

        /// Cleared and refilled per place, never freed.
        void clear()
        {
            mFrame.clear();
            mWait.clear();
            mWalk.clear();
            mPlace.clear();
        }

        /// What one measured frame cost, and the two shares of it this fork itself owns.
        void add(double frameMs, double walkMs, double placeMs)
        {
            mFrame.push_back(frameMs);
            mWalk.push_back(walkMs);
            mPlace.push_back(placeMs);
        }

        /// What the device reported for the frame behind, which arrives on its own schedule and on
        /// the first frames of a run does not arrive at all.
        void addWait(double waitMs) { mWait.push_back(waitMs); }

        bool empty() const { return mFrame.empty(); }
        std::uint32_t size() const { return static_cast<std::uint32_t>(mFrame.size()); }
    };

    /// Sorts `times` and summarises it. At least one time, which every caller has by construction.
    ///
    /// **Nearest rank, which is a sample and never an interpolation between two.** Every figure
    /// here is a frame that actually happened, so a percentile can be looked up in the run that
    /// produced it: the qth is the `ceil(q * n)`th shortest, counting from one.
    ///
    /// **By reference, and it sorts in place.** A copy would read better at the call site and
    /// cannot be had: taking one loses the compiler its proof that the caller's loop pushed at
    /// least once, and indexing a vector it can no longer see into is a hard warning.
    FrameTimes summarise(std::vector<double>& times);

    /// One stretch of the device's frame, over a run of frames.
    struct GpuZone
    {
        std::string_view mName;

        /// What the zone cost on the frames that ran it, which for an occasional pass is a
        /// distribution over a handful of frames and not over the run.
        FrameTimes mTimes;

        /// How many frames ran it, out of how many the run measured.
        std::uint32_t mFrames = 0;
        std::uint32_t mOfFrames = 0;

        /// What it cost the average frame: everything it spent, over every frame of the run.
        ///
        /// **The figure a report quotes, because it is the only one that can be summed or set
        /// against the frame beside it.** A pass that runs at a cell crossing and nowhere else is
        /// a median of the nineteen frames that crossed, and a row of those medians describes no
        /// frame that ever happened — `micromap 7.51` was printed above a frame median of 6.73.
        double mShareMs = 0.0;

        /// Whether every measured frame ran it, which is what says the share above is also what
        /// the zone costs on a frame.
        bool isEveryFrame() const { return mFrames == mOfFrames; }
    };

    /// Per-zone device times, gathered a frame at a time.
    ///
    /// **Kept in the order the zones first appeared**, which is the order the work was recorded:
    /// place the world, then trace it, then resolve it. A frame that skipped a pass — nothing moved,
    /// so nothing was placed — leaves that zone one sample short rather than shifting every zone
    /// after it into the wrong row.
    class GpuBreakdown
    {
    public:
        /// Takes one frame's zones. The names are the backend's literals and are copied on first
        /// sight only, so a long run pushes a double per zone and nothing else.
        ///
        /// **One call per measured frame, whether or not that frame reported a zone.** The count
        /// it keeps is the denominator every share below is taken over, so a frame handed to the
        /// report and not to this would make each of them larger than the frame it describes.
        void add(std::span<const GpuSpan> spans);

        /// Summarises what was gathered, the largest share of a frame first — which is the order
        /// the question "where did the frame go" wants read. Empty where no frame reported a zone.
        std::span<const GpuZone> summariseZones();

        bool empty() const { return mNames.empty(); }

    private:
        /// The backend's own literals — see `GpuSpan::mName` for why a view over one is kept.
        std::vector<std::string_view> mNames;

        /// One row of samples per name, indexed alongside `mNames`.
        std::vector<std::vector<double>> mTimes;

        /// Frames `add` was called for, which is what a zone's row is short against.
        std::uint32_t mFrames = 0;

        std::vector<GpuZone> mZones;
    };

    /// The column headings the rows below line up under.
    std::string describeHeadings();

    /// One row of six figures under `heading`, in the order `describeHeadings` names them.
    std::string describeTimes(std::string_view heading, const FrameTimes& times);

    /// One zone in a report: what it cost the average frame, and — where it did not run in every
    /// frame — what it cost when it did, on how many of them.
    ///
    /// **One spelling for both hosts**, because the harness and the game print the same zones and a
    /// figure read differently between the two reports is a difference read as a finding.
    std::string describeZone(const GpuZone& zone);

    /// The device's own account of the frame, shares of a frame only and largest first.
    ///
    /// **Six distributions would be a wall**; what this row answers is which stretch of the frame is
    /// the expensive one, and the rows above it already say how much the whole frame varies.
    std::string describeZones(std::span<const GpuZone> zones);
}
