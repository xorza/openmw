#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <components/rtx/renderer.hpp>

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

    /// Which of a measured frame's figures a row holds.
    ///
    /// **`Wait` is the CPU standing still for the device, `Walk` is the world being mirrored, and
    /// `Place` is the renderer being told what moved.** What is left of `Frame` is the frame's own
    /// record. Lumping them would hide which of them a place is slow because of; a wait near the
    /// frame is a device that cannot keep up, and a wait near nought is a CPU that cannot.
    ///
    /// **`Finish` is the whole of collecting the frame behind, and `Wait` is its largest share.**
    /// The fence is what `Wait` measures; what `Finish` adds is everything the ring does once the
    /// fence has passed — reading the device's counters and its timestamps, and destroying what
    /// that frame was the last to read. The two are apart because one is the device being slow and
    /// the other is this renderer being slow, and a frame can be either.
    ///
    /// **Five more of them are shares of another row rather than of the frame**, and they follow
    /// the row they are inside. `Warm` is how long the walk stood waiting for the terrain's warming
    /// thread, and `Fold` is what that walk spent folding the geometry which arrived on it — a
    /// median of nothing and three quarters of the walk at the worst frames of a route, because a
    /// paged chunk arrives as one merged geometry of every static of a kind in it.
    /// `Bake`, `Textures` and `Upload` are the three halves of `Place` that a spike could be in —
    /// the ground the composite queue handed back, the arrived textures being opened and described,
    /// and what the backend was then told. What is left of `Place` is the lists it reads either
    /// side of them.
    ///
    /// **`Trace` and `Present` are the other two calls the frame makes into the backend** — the
    /// record and its submit, and the picture reaching the surface with the interface over it.
    /// **`Update` is the rest of the loop and it is the game's**: the world stepping and the cells
    /// arriving, between one call into the renderer and the next.
    ///
    /// **All four are timed rather than profiled, because a profile cannot read them.** Most of
    /// what a call into the driver costs is inside the driver, which carries no frame pointer to
    /// walk, and a thread asleep is counted by a sampling profiler as nothing at all. Measured on
    /// the island route, perf reads the trace call at nine microseconds a frame and the wall clock
    /// reads it at a hundred and eighty.
    ///
    /// **Together they close the frame.** `Frame` less `Finish`, `Walk`, `Place`, `Trace`,
    /// `Present` and `Update` is under 0.05 ms at every place that stands still: what is left is the
    /// window's extent being handed over, the sweep, and the frame's own record. A row that does
    /// not close is a stretch nobody has named, which is what these were added to find.
    ///
    /// **Why `Place` needed splitting at all**: on the island route it is 0.18 ms at the median and
    /// 62 at the worst, and a profile cannot say which half — an arrival frame's time is in the
    /// driver, and the driver carries no frame pointer for perf to walk.
    enum class Timing : std::uint32_t
    {
        Frame,
        Finish,
        Wait,
        Walk,
        Warm,
        Fold,
        Place,
        Bake,
        Textures,
        Upload,
        Trace,
        Present,
        Update,
    };

    inline constexpr std::size_t sTimingCount = 13;

    /// What a report heads each row with, and — with `Ms` after it — what the JSON names it.
    inline constexpr std::array<std::string_view, sTimingCount> sTimingNames{ "frame", "finish", "wait", "walk", "warm",
        "fold", "place", "bake", "textures", "upload", "trace", "present", "update" };

    /// What one measured frame spent on the host, by phase.
    ///
    /// **One bag, because the list only ever grows.** Each of these arrives at the same call from a
    /// different place, and a signature that named them one by one was six parameters of one type
    /// in a row — which is six chances to hand them over in the wrong order and no way to be caught
    /// at it.
    struct FrameSpend
    {
        double mFinishMs = 0.0;
        double mWalkMs = 0.0;
        double mWarmMs = 0.0;
        double mFoldMs = 0.0;
        double mPlaceMs = 0.0;
        double mBakeMs = 0.0;
        double mTexturesMs = 0.0;
        double mUploadMs = 0.0;
        double mTraceMs = 0.0;
        double mPresentMs = 0.0;
        double mUpdateMs = 0.0;
    };

    inline constexpr std::size_t indexOf(const Timing timing)
    {
        return static_cast<std::size_t>(timing);
    }

    /// Every figure a measured frame contributes, gathered over one run.
    ///
    /// **One object because they are cleared, filled and summarised together.** Rows kept apart are
    /// a chance for a frame to reach all but one of them, and rows out of step with each other are
    /// rows that cannot be read against each other at all. **One array**, because named members are
    /// an edit apiece wherever a further figure is wanted.
    ///
    /// **Shared by the harness and the game**, whose two reports only mean something beside each
    /// other: a crossing in one is measured against a crossing in the other, and a row one of them
    /// gathered differently would be a difference read as a finding.
    struct FrameSamples
    {
        std::array<std::vector<double>, sTimingCount> mRows;

        std::vector<double>& at(const Timing timing) { return mRows[indexOf(timing)]; }
        const std::vector<double>& at(const Timing timing) const { return mRows[indexOf(timing)]; }

        void reserve(std::uint32_t frames)
        {
            for (std::vector<double>& row : mRows)
                row.reserve(frames);
        }

        /// Cleared and refilled per place, never freed.
        void clear()
        {
            for (std::vector<double>& row : mRows)
                row.clear();
        }

        /// What one measured frame cost, and the shares of it this fork itself owns.
        void add(double frameMs, const FrameSpend& spend)
        {
            at(Timing::Frame).push_back(frameMs);
            at(Timing::Finish).push_back(spend.mFinishMs);
            at(Timing::Walk).push_back(spend.mWalkMs);
            at(Timing::Warm).push_back(spend.mWarmMs);
            at(Timing::Fold).push_back(spend.mFoldMs);
            at(Timing::Place).push_back(spend.mPlaceMs);
            at(Timing::Bake).push_back(spend.mBakeMs);
            at(Timing::Textures).push_back(spend.mTexturesMs);
            at(Timing::Upload).push_back(spend.mUploadMs);
            at(Timing::Trace).push_back(spend.mTraceMs);
            at(Timing::Present).push_back(spend.mPresentMs);
            at(Timing::Update).push_back(spend.mUpdateMs);
        }

        /// What the device reported for the frame behind, which arrives on its own schedule and on
        /// the first frames of a run does not arrive at all.
        void addWait(double waitMs) { at(Timing::Wait).push_back(waitMs); }

        bool empty() const { return at(Timing::Frame).empty(); }
        std::uint32_t size() const { return static_cast<std::uint32_t>(at(Timing::Frame).size()); }
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
        /// frame that ever happened — `blas 7.51` was printed above a frame median of 6.73.
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
        /// sight only, so a long run pushes a double per zone and nothing else, and a zone opened
        /// more than once in the frame is summed into that frame's own sample.
        ///
        /// **One call per measured frame, whether or not that frame reported a zone.** The count
        /// it keeps is the denominator every share below is taken over, so a frame handed to the
        /// report and not to this would make each of them larger than the frame it describes.
        void add(std::span<const GpuSpan> spans);

        /// Summarises what was gathered, the largest share of a frame first — which is the order
        /// the question "where did the frame go" wants read. Empty where no frame reported a zone.
        std::span<const GpuZone> summariseZones();

        bool empty() const { return mRows.empty(); }

    private:
        /// One zone's name and what it has cost.
        ///
        /// **One row and not three vectors kept level by hand.** A zone met for the first time has
        /// to reach all three, and a name pushed without its row is an index that reads another
        /// zone's samples.
        struct ZoneRow
        {
            /// The backend's own literal — see `GpuSpan::mName` for why a view over one is kept.
            std::string_view mName;

            /// One sample a frame, never one a span.
            ///
            /// **Its own vector, because the rows are pushed to independently.** A zone runs on the
            /// frames it runs on, so a flat buffer shared by all of them would have to be laid out
            /// again whenever one outgrew its share — inside a frame it is timing.
            std::vector<double> mTimes;

            /// Which frame this was last given a sample on, so the spans of one frame land in one
            /// of them. Nought for a zone nothing has reported yet, which is an index no frame has.
            std::uint32_t mSeen = 0;
        };

        std::vector<ZoneRow> mRows;

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
