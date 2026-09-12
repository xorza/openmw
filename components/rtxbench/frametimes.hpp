#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <components/rtx/framespend.hpp>
#include <components/rtx/renderer.hpp>

namespace Rtx
{
    /// What the last second of frames came to, as one short line for a glance.
    ///
    /// **A second and not a frame**, because a figure that changes sixty times a second cannot be
    /// read. **The mean beside the worst**, because the mean alone is the figure that hides a
    /// stutter. A median would need the second's frames kept and sorted, and a sort landing on one
    /// frame in sixty is the spike a frame path is not allowed to carry.
    ///
    /// **Nothing here reaches the heap.** The line is formatted into a buffer this owns, so the
    /// frame that closes a second costs one format and every other costs three adds.
    class FrameRate
    {
    public:
        /// Takes one frame's wall time. True on the frame that closes a second's worth, at which
        /// point `getText` describes that second and the next one starts counting from nothing.
        bool add(double frameMs);

        /// The last closed second: `118 fps, 8.5 ms, worst 12.3 ms`. Empty until one has closed.
        std::string_view getText() const { return { mText.data(), mLength }; }

    private:
        double mSummedMs = 0.0;
        double mWorstMs = 0.0;
        std::uint32_t mFrames = 0;

        std::array<char, 64> mText{};
        std::size_t mLength = 0;
    };

    /// perf's control fifo, so a recording holds the frames that were measured and nothing else.
    ///
    /// `perf record --delay=-1 --control=fifo:<path>` starts with its counters off and turns them
    /// on when the word `enable` arrives on the fifo. Handed that path, this writes the word around
    /// each place's measured frames, and the profile is then those frames: not the engine starting,
    /// not a cell being read off the disk, not the renderer being taken down. Trimming a whole-run
    /// recording to a guessed boundary instead is the alternative, and it reports a quarter of its
    /// samples against code the benchmark never claimed to measure.
    ///
    /// An empty path does nothing at all, which is every run that is not being profiled.
    class PerfControl
    {
    public:
        explicit PerfControl(std::filesystem::path fifo);
        ~PerfControl();

        PerfControl(const PerfControl&) = delete;
        PerfControl& operator=(const PerfControl&) = delete;

        /// Starts counting. The first call opens the fifo.
        void enable();

        /// Stops counting. Silent before the first `enable`, so a run stopped early is not an error.
        void disable();

    private:
        /// Opens the fifo for writing, once.
        ///
        /// **Not on construction, because the reader has to be there first.** Opening a fifo for
        /// writing with nobody reading it fails outright under `O_NONBLOCK` and blocks forever
        /// without it, and perf attaches to an already-running process seconds after it started.
        /// Deferring to the first `enable` puts the open after a cell has been read, by which time
        /// perf has long since opened its end.
        void connect();

        void send(std::string_view command);

        std::filesystem::path mFifo;
        int mHandle = -1;
    };

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
        ///
        /// **A loop over the rows and not a line per figure.** `Timing::Frame` is the caller's and
        /// `Timing::Wait` arrives on its own schedule, so those two are the only ones named here.
        void add(double frameMs, const FrameSpend& spend)
        {
            for (const Timing timing : sTimings.values())
                if (timing != Timing::Frame && timing != Timing::Wait)
                    at(timing).push_back(spend.at(timing));

            at(Timing::Frame).push_back(frameMs);
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
