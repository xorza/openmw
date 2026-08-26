#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "renderer.hpp"

namespace Rtx
{
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
        std::string mName;
        FrameTimes mTimes;
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
        void add(std::span<const GpuSpan> spans);

        /// Summarises what was gathered, most expensive first — which is the order the question
        /// "where did the frame go" wants read. Empty where no frame reported a zone.
        std::span<const GpuZone> summariseZones();

        bool empty() const { return mNames.empty(); }

    private:
        std::vector<std::string> mNames;

        /// One row of samples per name, indexed alongside `mNames`.
        std::vector<std::vector<double>> mTimes;

        std::vector<GpuZone> mZones;
    };

    /// The column headings the rows below line up under.
    std::string describeHeadings();

    /// One row of six figures under `heading`, in the order `describeHeadings` names them.
    std::string describeTimes(std::string_view heading, const FrameTimes& times);

    /// The device's own account of the frame, medians only and most expensive first.
    ///
    /// **Six distributions would be a wall**; what this row answers is which stretch of the frame is
    /// the expensive one, and the rows above it already say how much the whole frame varies.
    std::string describeZones(std::span<const GpuZone> zones);
}
