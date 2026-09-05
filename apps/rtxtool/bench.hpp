#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <components/rtx/renderer.hpp>
#include <components/rtxbench/benchrecord.hpp>
#include <components/rtxbench/benchspec.hpp>

#include "framerequest.hpp"
#include "views.hpp"

namespace Rtx
{
    struct ValidationOptions;
}

namespace RtxTool
{
    class World;

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

        /// How long each place runs and how much of it is thrown away first, in seconds of world
        /// or in frames — `Rtx::BenchSpec`, which is the one spelling both hosts read.
        ///
        /// **A warm-up because this machine's GPU idles at 315 MHz and ramps under load**, and the
        /// first submits of a scene also pay for its residency. A cold frame has timed five times a
        /// warm one, which is wider than most changes worth measuring.
        Rtx::BenchSpec mSpec{ .mRun = { .mSeconds = 10.0f }, .mWarm = { .mSeconds = 1.0f } };

        /// Whether the run is shown while it happens. A window presents through a mailbox
        /// swapchain, so it does not pace the loop; what it costs is one present per frame, and it
        /// is the only way to see that a place is being profiled facing a wall.
        bool mWindow = true;

        /// Weathers to turn the sky through while a place runs, in order and round again.
        ///
        /// **The one thing the game does constantly that nothing here could do.** A transition is
        /// not two weathers taken in turn: the sky is blended between them, and the precipitation
        /// of the one arriving replaces the one leaving partway through — so every mesh and
        /// texture an emitter brought goes while the camera is moving and cells are landing.
        /// `view`'s weather keys were the only path to it, and a window nobody is typing at cannot
        /// be driven.
        ///
        /// **Asking for it stops the run being a benchmark**, for the reason the hashes give: no
        /// two places stand under the same sky, so the rows are comparable with nothing. Empty
        /// holds one weather, which is what a measurement wants.
        std::vector<std::string> mTurnWeather;

        /// Whether a local-map tile of the camera's own cell is traced on every frame.
        ///
        /// **What the game's compass does and no other path here ever has.** A picture of the world
        /// is traced between the placement and the frame, against the scene the placement has just
        /// written and the copy of the tables it wrote — `Rtx::Renderer::traceGuiTexture`, on the
        /// world's slot. Every bench before this drew the frame and nothing else, so that whole
        /// half of the renderer was reached only by `map` and `doll`, one still picture at a time,
        /// with no cell arriving and no weather turning under it.
        ///
        /// **Asking for it stops the run being a benchmark**: a second trace a frame is most of a
        /// second frame.
        bool mMapTile = false;
    };

    /// Runs `request` and reports. Returns a process exit status.
    int runBench(World& world, const Rtx::ValidationOptions& validation, const BenchRequest& request);
}
