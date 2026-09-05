#include "benchrecord.hpp"

#include <cmath>
#include <cstddef>
#include <format>
#include <fstream>

namespace Rtx
{
    namespace
    {
        double megabytes(std::uint64_t bytes)
        {
            return static_cast<double>(bytes) / (1024.0 * 1024.0);
        }

        /// Null where nothing answered, so a record taken on a machine with no `nvidia-smi` says it
        /// carries no clock rather than claiming one of zero.
        std::string asJson(const GpuClock& clock)
        {
            if (!clock.mRead)
                return "null";

            return std::format(
                R"({{"lowestMhz": {}, "highestMhz": {}, "memoryMhz": {}, "temperatureC": {}, "throttle": "{}"}})",
                clock.mLowestMhz, clock.mHighestMhz, clock.mMemoryMhz, clock.mTemperatureC,
                describeThrottle(clock.mThrottleMask));
        }

        /// Everything a scene came to, so the record can compare what a change cost in memory as
        /// well as in time.
        ///
        /// **Every field, because the report beside it chooses and this does not.** A human report
        /// leaves out what nobody reads at a glance; a record exists to be diffed against the same
        /// run on another commit, and a figure it never wrote is one nobody can go back for.
        std::string asJson(const SceneStats& scene)
        {
            return std::format(R"({{"instances": {}, "cutoutInstances": {}, "micromappedInstances": {}, )"
                               R"("structureBytes": {}, "micromapBytes": {}, "tableBytes": {}, "textureCount": {}, )"
                               R"("textureBytes": {}}})",
                scene.mInstances, scene.mCutoutInstances, scene.mMicromappedInstances, scene.mStructureBytes,
                scene.mMicromapBytes, scene.mTableBytes, scene.mTextureCount, scene.mTextureBytes);
        }

        std::string asJson(const Crossings& crossings)
        {
            return std::format(
                R"({{"count": {}, "rebuilds": {}, "worstMs": {:.2f}, "readMs": {:.2f}, "buildMs": {:.2f}}})",
                crossings.mCount, crossings.mRebuilds, crossings.mWorstMs, crossings.mReadMs, crossings.mBuildMs);
        }

        std::string asJson(const FrameTimes& times)
        {
            return std::format(
                R"({{"median": {:.4f}, "mean": {:.4f}, "p95": {:.4f}, "p99": {:.4f}, "best": {:.4f}, "worst": {:.4f}}})",
                times.mMedian, times.mMean, times.mP95, times.mP99, times.mBest, times.mWorst);
        }

        /// **The counts as well as the times**, because a zone's distribution is over the frames
        /// that ran it: without them a record cannot tell a pass that costs the frame a tenth of a
        /// millisecond from one that costs seven every sixtieth frame.
        std::string asJson(const GpuZone& zone)
        {
            return std::format(R"({{"shareMs": {:.4f}, "frames": {}, "ofFrames": {}, "times": {}}})", zone.mShareMs,
                zone.mFrames, zone.mOfFrames, asJson(zone.mTimes));
        }
    }

    std::string describeHour(const float hour)
    {
        const int minutes = static_cast<int>(std::lround(hour * 60.0f)) % (24 * 60);
        return std::format("{:02}:{:02}", minutes / 60, minutes % 60);
    }

    std::string describePlace(const BenchPlace& place)
    {
        std::string out;

        // **Each line only where there is something to put in it.** A place a harness staged names
        // a view, a cell and a scene it built; a game measuring itself names none of the three, and
        // a row of empty quotes and zeroes reads as a measurement of nothing rather than as an
        // absence.
        if (!place.mView.empty())
        {
            out += '\n' + place.mView;
            if (!place.mNote.empty())
                out += " — " + place.mNote;

            out += '\n';
        }

        if (!place.mCell.empty())
            out += std::format(
                "  cell {} at {} in {}   {} instances ({} cutouts, {} micromapped)   {:.1f} MiB structures, "
                "{:.1f} MiB micromaps   {} textures, {:.1f} MiB\n",
                place.mCell, describeHour(place.mHour), place.mWeather, place.mScene.mInstances,
                place.mScene.mCutoutInstances, place.mScene.mMicromappedInstances,
                megabytes(place.mScene.mStructureBytes), megabytes(place.mScene.mMicromapBytes),
                place.mScene.mTextureCount, megabytes(place.mScene.mTextureBytes));

        // **Two facts and not one line.** A staged place pays one build before its frames and can
        // name what it cost; a run of a real game builds a little at every crossing and has no such
        // number. What both have is how much of the frame hit something, which is what tells "the
        // cell rendered" from "the camera faced away from it".
        if (place.mBuildMs > 0.0)
            out += std::format("  build {:.0f} ms\n", place.mBuildMs);

        if (place.mHitPercent > 0.0)
            out += std::format("  {:.1f}% of primary rays hit\n", place.mHitPercent);

        out += describeHeadings();
        out += describeTimes("frame ms", place.mFrame);
        out += describeTimes("wait ms", place.mWait);
        out += describeTimes("walk ms", place.mWalk);
        out += describeTimes("place ms", place.mPlace);

        // **The device's own account of the same frame, one figure each.** Six distributions would
        // be a wall; what this row answers is "which of them is the expensive one", and the row
        // above already says how much the whole frame varies. Each figure is the zone's share of
        // the average frame, so the row sums to the device's part of it and a pass that only runs
        // at a crossing says so beside its own share.
        out += describeZones(place.mGpu);
        out += describeClock(place.mClock);

        // **Only for a route, because a place that stands still has nothing to say here.** The
        // worst is the one to read: a crossing is a dropped frame, and an average over six hundred
        // frames of which four were the expensive ones hides exactly the thing.
        if (place.mCrossings.mCount > 0)
            out += std::format(
                "  {} crossings, {} of them rebuilds — {:.0f} ms worst; {:.1f} s over the run, "
                "{:.1f} reading and {:.1f} building{}\n",
                place.mCrossings.mCount, place.mCrossings.mRebuilds, place.mCrossings.mWorstMs,
                (place.mCrossings.mReadMs + place.mCrossings.mBuildMs) / 1000.0, place.mCrossings.mReadMs / 1000.0,
                place.mCrossings.mBuildMs / 1000.0,
                place.mTravelled < 1.0 ? std::format(", {:.0f}% of the route flown", place.mTravelled * 100.0) : "");

        out += std::format("  {} frames in {:.2f} s — {:.1f} fps, {:.1f} at the 1% low\n", place.mFrames,
            place.mWallSeconds, place.mFrame.getRate(), place.mFrame.getLowRate());

        return out;
    }

    std::string describeTotal(std::span<const BenchPlace> places, const bool stopped)
    {
        if (places.size() < 2 && !stopped)
            return {};

        std::uint32_t frames = 0;
        double lasted = 0.0;
        for (const BenchPlace& place : places)
        {
            frames += place.mFrames;
            lasted += place.mWallSeconds;
        }

        return std::format("\n{} {}, {} frames in {:.1f} s{}\n", places.size(), places.size() == 1 ? "place" : "places",
            frames, lasted, stopped ? " — stopped early" : "");
    }

    void writeJson(
        const std::filesystem::path& path, const BenchHeader& header, const std::span<const BenchPlace> places)
    {
        std::ofstream file(path);

        file << "{\n"
             << std::format(R"(  "suite": "{}",)", header.mSuite) << '\n'
             << std::format(R"(  "output": [{}, {}],)", header.mExtents.mOutputWidth, header.mExtents.mOutputHeight)
             << '\n'
             << std::format(R"(  "render": [{}, {}],)", header.mExtents.mRenderWidth, header.mExtents.mRenderHeight)
             << '\n'
             << std::format(R"(  "upscale": "{}",)", upscaleName(header.mUpscale)) << '\n'
             << std::format(R"(  "preset": "{}",)", presetName(header.mPreset)) << '\n'
             << std::format(R"(  "reorder": "{}",)", reorderName(header.mReorder)) << '\n'
             << std::format(R"(  "frames": {}, "warmup": {}, "validation": {},)", header.mMeasured, header.mWarmup,
                    header.mValidating)
             << '\n'
             << R"(  "places": [)" << '\n';

        for (std::size_t at = 0; at < places.size(); ++at)
        {
            const BenchPlace& place = places[at];
            file << std::format(R"(    {{"view": "{}", "cell": "{}", "hour": {}, "weather": "{}", )", place.mView,
                place.mCell, place.mHour, place.mWeather)
                 << std::format(R"("buildMs": {:.2f}, )", place.mBuildMs) << R"("scene": )" << asJson(place.mScene)
                 << std::format(R"(, "frames": {}, "wallSeconds": {:.4f}, "hitPercent": {:.2f}, )", place.mFrames,
                        place.mWallSeconds, place.mHitPercent)
                 << R"("crossings": )" << asJson(place.mCrossings)
                 << std::format(R"(, "travelled": {:.4f}, )", place.mTravelled) << R"("frameMs": )"
                 << asJson(place.mFrame) << R"(, "waitMs": )" << asJson(place.mWait) << R"(, "walkMs": )"
                 << asJson(place.mWalk) << R"(, "placeMs": )" << asJson(place.mPlace) << R"(, "gpuMs": {)";

            for (std::size_t zone = 0; zone < place.mGpu.size(); ++zone)
                file << std::format(
                    R"({}"{}": {})", zone == 0 ? "" : ", ", place.mGpu[zone].mName, asJson(place.mGpu[zone]));

            file << "}, \"clock\": " << asJson(place.mClock) << "}" << (at + 1 < places.size() ? "," : "") << '\n';
        }

        file << "  ]\n}\n";
    }
}
