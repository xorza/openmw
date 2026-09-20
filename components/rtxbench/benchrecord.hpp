#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include <components/rtx/framespend.hpp>
#include <components/rtx/guirenderer.hpp>
#include <components/rtx/memoryreport.hpp>
#include <components/rtx/reconstruction.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtx/upscale.hpp>

#include "frametimes.hpp"
#include "gpuclock.hpp"
#include "threadwatch.hpp"

namespace Rtx
{
    /// How many cell boundaries a route crossed, and what the frames that crossed them cost.
    ///
    /// **A count and totals rather than a distribution**, because a run of six hundred frames
    /// crosses a couple of dozen: percentiles over that say nothing, and the number worth reading is
    /// the worst one — that is the frame a player feels. The whole cost is in `BenchPlace::mFrame`
    /// too, which is where it belongs: a crossing is not a separate budget, it is the frame that
    /// dropped. The whole frame and not a split into reading and building, because the game gives
    /// none: the ring arrives on the loading threads.
    struct Crossings
    {
        std::uint32_t mCount = 0;

        /// How many of those could not be appended to and cost a full build.
        ///
        /// **The single most useful number a route produces.** An append builds the structures the
        /// ring brought; a rebuild builds every structure in the scene and re-describes the whole
        /// texture table, which is append-only and has been growing since the run started. Which
        /// one a crossing gets is decided by whether the sweep found anything to drop, so a town
        /// appends and open country rebuilds — and the two are an order of magnitude apart.
        std::uint32_t mRebuilds = 0;

        double mWorstMs = 0.0;
        double mTotalMs = 0.0;

        /// Counts one crossing, from what the frame that crossed took.
        void add(bool rebuilt, double frameMs)
        {
            ++mCount;
            mRebuilds += rebuilt ? 1u : 0u;
            mTotalMs += frameMs;
            mWorstMs = std::max(mWorstMs, frameMs);
        }
    };

    /// How many frames the ring held at each submit over a run — `FrameResult::mInFlight`. The
    /// least and the mean, because the figure is one or two: a place that stands still holds the
    /// same number on every frame, and a route that drained the ring for an arrival holds one on
    /// that frame and two on the rest.
    /// One frame of a run, kept for the report because it was among the worst: how long it took,
    /// how many meshes its upload built structures for, and where the time went.
    struct WorstFrame
    {
        double mFrameMs = 0.0;
        std::uint32_t mArrivedMeshes = 0;
        FrameSpend mSpend;
    };

    /// The frames whose upload extended the scene — a cell handed over, an actor entering with a
    /// mesh nobody wore — and what they cost beside the rest, with the worst frames of the run
    /// whatever they carried. What says whether the tail is the arrivals': a structure built on
    /// the frame's own queue is a cost the frame that needed it pays, and this is the reading that
    /// says how much, before anything is moved off it.
    struct Arrivals
    {
        /// Frames whose upload extended or rebuilt the scene, and the meshes they brought.
        std::uint32_t mFrames = 0;
        std::uint32_t mMeshes = 0;

        /// What those frames came to.
        double mWorstMs = 0.0;
        double mSumMs = 0.0;

        /// The worst frames of the run, longest first, arrivals or not.
        static constexpr std::size_t sKept = 3;
        std::array<WorstFrame, sKept> mWorst{};
        std::size_t mWorstCount = 0;

        void add(double frameMs, std::uint32_t arrivedMeshes, const FrameSpend& spend);

        double getMeanMs() const { return mFrames == 0 ? 0.0 : mSumMs / mFrames; }
    };

    struct Overlap
    {
        std::uint32_t mLeast = 0;
        std::uint32_t mFrames = 0;
        double mSum = 0.0;

        void add(std::uint32_t inFlight)
        {
            mLeast = mFrames == 0 ? inFlight : std::min(mLeast, inFlight);
            ++mFrames;
            mSum += inFlight;
        }

        double getMean() const { return mFrames == 0 ? 0.0 : mSum / mFrames; }
    };

    /// What the hold's own clock said on each frame of a run that held — `FrameResult::mHeldMs`.
    /// The shortest and the longest, because the question is whether every frame held for as long
    /// as asked, and the longest is where a stall inside the loop shows.
    struct HoldTimes
    {
        double mShortestMs = 0.0;
        double mLongestMs = 0.0;
        std::uint32_t mFrames = 0;

        void add(double heldMs)
        {
            mShortestMs = mFrames == 0 ? heldMs : std::min(mShortestMs, heldMs);
            mLongestMs = std::max(mLongestMs, heldMs);
            ++mFrames;
        }
    };

    /// What one place came to.
    struct BenchPlace
    {
        std::string mView;
        std::string mCell;
        std::string mNote;

        /// The hour and the sky it stood under. Reported because they are most of what a frame
        /// costs: a low sun makes every shadow ray long and grazing, an overcast takes the sun out
        /// of half the frame, and two rows under different conditions are not comparable.
        float mHour = 12.0f;
        std::string mWeather;

        std::uint32_t mFrames = 0;
        double mWallSeconds = 0.0;

        /// The whole per-frame cost and the three shares of it worth telling apart, indexed by
        /// `Timing` — which is where the four are named and where a fifth would be.
        std::array<FrameTimes, sTimingCount> mRows;

        const FrameTimes& at(const Timing timing) const { return mRows[indexOf(timing)]; }
        FrameTimes& at(const Timing timing) { return mRows[indexOf(timing)]; }

        /// What the device itself says each stretch of the frame cost, most expensive first. Empty
        /// where the device cannot write timestamps.
        std::vector<GpuZone> mGpu;

        /// What the card was clocked at as this place ended. Every GPU figure above is at that
        /// clock, and two runs taken at different ones are not an A/B.
        GpuClock mClock;

        /// The busiest of the process's other threads across the place's frames, warm-up
        /// included: what says the driver's second compile of the launches (`CodeSettle`) did not
        /// run inside them, which would be a place measured on two codes. Not in the JSON, which
        /// is for comparing frame times across commits.
        ThreadShare mThreads;

        /// What fraction of primary rays hit something, as a percentage. A place profiled facing a
        /// wall is fast and means nothing, and this is what says so without opening a window.
        double mHitPercent = 0.0;

        Crossings mCrossings;

        Arrivals mArrivals;

        Overlap mOverlap;

        /// How far along its route the camera got, as a fraction. One where it arrived, or where
        /// the route named no destination, and less where the run ended first — a route flown too
        /// slowly to finish is measuring a shorter journey than it reads as.
        double mTravelled = 1.0;

        SceneStats mScene;

        /// What the device gave up for that scene, per heap.
        ///
        /// **A place and not a run**, because a route arrives at cells the last one did not: what
        /// the allocators reserve is a high-water mark, so the figure belongs to the place the run
        /// had reached when it was taken.
        MemoryReport mMemory;
    };

    /// What every place of a run stood under, for the record's own header.
    struct BenchHeader
    {
        /// Which suite this came from. Empty where the places were named on the command line, and
        /// where the run is the game measuring itself.
        std::string mSuite;

        FrameExtents mExtents;

        /// What upscaled the run's frames, as `Reconstruction` reports it: the mode and the network,
        /// or `Off` and `Default` where nothing did. **Read off a frame and not off the renderer**,
        /// which answers the mode alone.
        Upscaling mUpscaling{ .mMode = Upscale::Off, .mPreset = Preset::Default };

        std::uint32_t mMeasured = 0;
        std::uint32_t mWarmup = 0;

        /// Whether the layers were running, which is what says a figure is not one to quote.
        bool mValidating = false;
    };

    /// An hour of Morrowind's day as a person reads it, on a twenty-four hour clock.
    ///
    /// **One spelling, because three places print one.** A bench row, a view listing and the block
    /// a window prints all name the hour a frame stood at, and a row that spelled it differently
    /// would be a row nobody could search for.
    std::string describeHour(float hour);

    /// One place as the report prints it: what it stood in, what it was built out of, the four
    /// distributions, the device's own account, the clock and the frame rate.
    ///
    /// **Built whole and returned rather than streamed**, because the game logs its report and a
    /// table split across log lines by a timestamp apiece is not one.
    std::string describePlace(const BenchPlace& place);

    /// What a whole run came to, under the places. Empty for a run of one place, which has already
    /// said everything this would.
    std::string describeTotal(std::span<const BenchPlace> places, bool stopped);

    /// Writes the run as one record, for comparing against the same run on another commit.
    ///
    /// Hand-written rather than through a library: this is numbers and the names of places, and the
    /// alternative is a dependency for the sake of a page.
    void writeJson(const std::filesystem::path& path, const BenchHeader& header, std::span<const BenchPlace> places);
}
