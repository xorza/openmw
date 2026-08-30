#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <components/rtx/frametimes.hpp>
#include <components/rtx/renderer.hpp>

#include "framerequest.hpp"
#include "gpuclock.hpp"
#include "views.hpp"

namespace Rtx
{
    struct ValidationOptions;
}

namespace RtxTool
{
    class World;

    /// How many cell boundaries a route crossed, and what the rings cost.
    ///
    /// **A count and totals rather than a distribution**, because a run of six hundred frames
    /// crosses a couple of dozen: percentiles over that say nothing, and the number worth reading is
    /// the worst one — that is the frame a player feels. The whole cost is in `BenchPlace::mFrame`
    /// too, which is where it belongs: a crossing is not a separate budget, it is the frame that
    /// dropped.
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

        /// **Split, because the two halves are fixed by different work.** Reading is the content
        /// files, the models instanced out of them and the terrain chunks built — which the game
        /// hides behind `CellPreloader`'s threads and this harness deliberately does not. Building
        /// is what the renderer then does with what arrived, and is the only half this fork can fix
        /// in `components/rtx`.
        double mReadMs = 0.0;
        double mBuildMs = 0.0;

        /// Counts one crossing, from what its two halves took.
        void add(bool rebuilt, double readMs, double buildMs)
        {
            ++mCount;
            mRebuilds += rebuilt ? 1u : 0u;
            mReadMs += readMs;
            mBuildMs += buildMs;
            mWorstMs = std::max(mWorstMs, readMs + buildMs);
        }
    };

    /// The four figures a measured frame contributes, gathered over one place.
    ///
    /// **One object because they are cleared, filled and summarised together.** Four vectors kept
    /// apart are four chances for a frame to reach three of them, and rows out of step with each
    /// other are rows that cannot be read against each other at all.
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

        /// What one measured frame cost, and the two shares of it the harness itself owns.
        void add(double frameMs, double walkMs, double placeMs)
        {
            mFrame.push_back(frameMs);
            mWalk.push_back(walkMs);
            mPlace.push_back(placeMs);
        }

        /// What the device reported for the frame behind, which arrives on its own schedule and on
        /// the first frames of a place does not arrive at all.
        void addWait(double waitMs) { mWait.push_back(waitMs); }

        bool empty() const { return mFrame.empty(); }
        std::uint32_t size() const { return static_cast<std::uint32_t>(mFrame.size()); }
    };

    /// A profiling run, over a list of places.
    struct BenchRequest
    {
        FrameRequest mFrame;

        /// Which suite this came from, for the report and the record. Empty where the views were
        /// named on the command line instead.
        std::string mSuite;

        /// The places to run, in the order they are run in.
        std::vector<View> mViews;

        /// Where to write the run as a record, or empty for none.
        std::filesystem::path mJson;

        /// Where to write one hash a frame, and a previous run's hashes to compare this one with.
        /// Either is empty where it was not asked for.
        ///
        /// **Asking for either stops the run being a benchmark.** A hash reads the frame back, and
        /// a read back submits a copy and waits on it — so every frame is serialised against the
        /// device and the times the run prints measure that instead. It says so where it prints
        /// them. `FrameHashes` says what this is for.
        std::filesystem::path mHashes;
        std::filesystem::path mAgainst;

        /// perf's control fifo, or empty where the run is not being profiled.
        ///
        /// A recording bounded by this holds the measured frames of every place and nothing
        /// between them, so what the profile attributes time to is what the report's figures came
        /// from. See `PerfControl`.
        std::filesystem::path mPerfControl;

        /// How many seconds of *world* each place is run for.
        ///
        /// **World time and not wall time, which is the whole of what makes two runs comparable.**
        /// The world steps a sixtieth of a second per frame however long the frame took, so ten
        /// seconds is six hundred frames — the same six hundred frames, with the same particles in
        /// the same places and the same sample in each pixel, on a build that draws them in four
        /// seconds and on one that takes twenty. A run against the clock would animate further on
        /// the faster build and measure a different scene.
        float mSeconds = 10.0f;

        /// Frames drawn and thrown away before each place is measured, in seconds of world.
        ///
        /// **This machine's GPU idles at 315 MHz and ramps under load**, and the first submits of a
        /// scene also pay for its residency. A cold frame has timed five times a warm one, which is
        /// wider than most changes worth measuring.
        float mWarmup = 1.0f;

        /// Measured frames per place, overriding `mSeconds` where it is not zero.
        std::uint32_t mFrames = 0;

        /// Whether the run is shown while it happens. A window presents through a mailbox
        /// swapchain, so it does not pace the loop; what it costs is one present per frame, and it
        /// is the only way to see that a place is being profiled facing a wall.
        bool mWindow = true;

        /// How many frames each place is measured over, which is `mFrames` or what `mSeconds` comes
        /// to at the rate the world steps.
        std::uint32_t getMeasured() const;

        /// How many are drawn and discarded first.
        std::uint32_t getWarmup() const;
    };

    /// What one place came to.
    struct BenchPlace
    {
        std::string mView;
        std::string mCell;
        std::string mNote;

        /// The hour it stood at. Reported because it is most of what a frame costs: a low sun makes
        /// every shadow ray long and grazing, and two rows at different hours are not comparable.
        float mHour = sDefaultHour;

        /// What `setScene` cost: every bottom-level structure built and every texture uploaded.
        /// The same cost a cell arriving in the game pays.
        double mBuildMs = 0.0;

        std::uint32_t mFrames = 0;
        double mWallSeconds = 0.0;

        /// The whole per-frame cost, and the three shares of it worth telling apart.
        ///
        /// **`mWait` is the CPU standing still for the device, `mWalk` is the harness standing in
        /// for the game, and `mPlace` is the renderer being told what moved** — posing the actors,
        /// running the emitters and walking the graph again, and then the top level rebuilt with
        /// every skinned mesh's structure refitted. What is left over is the frame's own record.
        /// Lumping them would hide which of them a place is slow because of; a wait near the frame
        /// is a device that cannot keep up, and a wait near nought is a CPU that cannot.
        ///
        /// **The walk is a row because it is the largest CPU cost of a streaming frame**, and it is
        /// the one cost here the game pays as well — it walks its own graph every frame. Inside
        /// `mFrame` it is a figure only a profile finds.
        Rtx::FrameTimes mFrame;
        Rtx::FrameTimes mWait;
        Rtx::FrameTimes mWalk;
        Rtx::FrameTimes mPlace;

        /// What the device itself says each stretch of the frame cost, most expensive first. Empty
        /// where the device cannot write timestamps.
        std::vector<Rtx::GpuZone> mGpu;

        /// What the card was clocked at as this place ended. Every GPU figure above is at that
        /// clock, and two runs taken at different ones are not an A/B.
        GpuClock mClock;

        /// What fraction of primary rays hit something, as a percentage. A place profiled facing a
        /// wall is fast and means nothing, and this is what says so without opening a window.
        double mHitPercent = 0.0;

        Crossings mCrossings;

        /// How far along its route the camera got, as a fraction. One where it arrived, and less
        /// where the run ended first — a route flown too slowly to finish is measuring a shorter
        /// journey than it reads as.
        double mTravelled = 0.0;

        Rtx::SceneStats mScene;
    };

    /// Runs `request` and reports. Returns a process exit status.
    int runBench(World& world, const Rtx::ValidationOptions& validation, const BenchRequest& request);
}
