#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include <components/rtx/frametimes.hpp>
#include <components/rtx/reconstruction.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtx/reorder.hpp>
#include <components/rtx/upscale.hpp>

#include "gpuclock.hpp"

namespace Rtx
{
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
        /// files, the models instanced out of them and the terrain chunks built. Building is what
        /// the renderer then does with what arrived, and is the only half this fork can fix in
        /// `components/rtx`.
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

        /// What the first hand-over cost: every bottom-level structure built and every texture
        /// uploaded. The same cost a cell arriving in the game pays.
        double mBuildMs = 0.0;

        std::uint32_t mFrames = 0;
        double mWallSeconds = 0.0;

        /// The whole per-frame cost, and the three shares of it worth telling apart.
        ///
        /// **`mWait` is the CPU standing still for the device, `mWalk` is the world being mirrored,
        /// and `mPlace` is the renderer being told what moved.** What is left over is the frame's
        /// own record. Lumping them would hide which of them a place is slow because of; a wait
        /// near the frame is a device that cannot keep up, and a wait near nought is a CPU that
        /// cannot.
        FrameTimes mFrame;
        FrameTimes mWait;
        FrameTimes mWalk;
        FrameTimes mPlace;

        /// What the device itself says each stretch of the frame cost, most expensive first. Empty
        /// where the device cannot write timestamps.
        std::vector<GpuZone> mGpu;

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
        double mTravelled = 1.0;

        SceneStats mScene;
    };

    /// What every place of a run stood under, for the record's own header.
    struct BenchHeader
    {
        /// Which suite this came from. Empty where the places were named on the command line, and
        /// where the run is the game measuring itself.
        std::string mSuite;

        FrameExtents mExtents;
        Upscale mUpscale = Upscale::Off;
        Preset mPreset = Preset::D;
        Reorder mReorder = Reorder::Off;

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
