#include "benchrecord.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <fstream>
#include <string>

#include <components/rtx/framespend.hpp>
#include <components/rtx/memoryreport.hpp>

namespace Rtx
{
    namespace
    {
        /// Null where nothing answered, so a record taken on a machine with no driver library says
        /// it carries no clock rather than claiming one of zero.
        std::string asJson(const GpuClock& clock)
        {
            if (!clock.mRead)
                return "null";

            return std::format(
                R"({{"lowestMhz": {}, "highestMhz": {}, "memoryMhz": {}, "temperatureC": {}, "throttle": "{}"}})",
                clock.mLowestMhz, clock.mHighestMhz, clock.mMemoryMhz, clock.mTemperatureC,
                describeThrottle(clock.mThrottleMask));
        }

        /// Null where nothing looked, for the same reason; and in the record at all because a
        /// record is what a run on another commit is read against, and a run another process
        /// drew through is not the same run.
        std::string asJson(const CardShare& card)
        {
            if (!card.mViewed)
                return "null";

            std::string holders = "[";
            for (std::size_t at = 0; at < card.mHolders.size(); ++at)
                holders += std::format(R"({}{{"name": "{}", "samples": {}}})", at == 0 ? "" : ", ",
                    card.mHolders[at].mName, card.mHolders[at].mSamples);
            holders += ']';

            return std::format(R"({{"seconds": {:.2f}, "samples": {}, "others": {}, "holders": {}}})", card.mSeconds,
                card.mSamples, card.mOthers, holders);
        }

        /// Everything a scene came to, so the record can compare what a change cost in memory as
        /// well as in time.
        ///
        /// **Every field, because the report beside it chooses and this does not.** A human report
        /// leaves out what nobody reads at a glance; a record exists to be diffed against the same
        /// run on another commit, and a figure it never wrote is one nobody can go back for.
        std::string asJson(const SceneStats& scene)
        {
            return std::format(R"({{"instances": {}, "cutoutInstances": {}, )"
                               R"("waterInstances": {}, "mediumInstances": {}, )"
                               R"("structureBytes": {}, "structureLiveBytes": {}, )"
                               R"("compactableBytes": {}, "compactableNowBytes": {}, "rebuilt": {}, )"
                               R"("tableBytes": {}, "textureCount": {}, "textureBytes": {}}})",
                scene.mInstances.mPlaced, scene.mInstances.mCutout, scene.mInstances.mWater, scene.mInstances.mMedium,
                scene.mStructureBytes, scene.mStructureLiveBytes, scene.mCompactableBytes, scene.mCompactableNowBytes,
                scene.mRebuilt, scene.mTableBytes, scene.mTextureCount, scene.mTextureBytes);
        }

        std::string asJson(const Arrivals& arrivals)
        {
            std::string worst = "[";
            for (std::size_t at = 0; at < arrivals.mWorstCount; ++at)
                worst += std::format(R"({}{{"frameMs": {:.2f}, "arrivedMeshes": {}}})", at == 0 ? "" : ", ",
                    arrivals.mWorst[at].mFrameMs, arrivals.mWorst[at].mArrivedMeshes);
            worst += ']';

            return std::format(R"({{"frames": {}, "meshes": {}, "worstMs": {:.2f}, "meanMs": {:.2f}, "worst": {}}})",
                arrivals.mFrames, arrivals.mMeshes, arrivals.mWorstMs, arrivals.getMeanMs(), worst);
        }

        /// One of the worst frames, as the place's report prints it: the whole frame, what it
        /// brought, and the three largest spends in it — enough to say whose the frame was.
        std::string describeWorst(const WorstFrame& frame)
        {
            // The stretches that sum to the frame, and not the frame itself nor a share of one
            // of them: `Timing` says `Wait` is most of `Finish` and `Upload` most of `Place`, so
            // a share beside its whole is one stretch printed twice.
            constexpr std::array<Timing, 7> sNotStretches{ Timing::Frame, Timing::Wait, Timing::Fold, Timing::Bake,
                Timing::Textures, Timing::Upload, Timing::Sleep };
            std::array<Timing, sTimingCount> spends = sTimings.values();
            const auto end = std::remove_if(spends.begin(), spends.end(), [&](const Timing timing) {
                return std::find(sNotStretches.begin(), sNotStretches.end(), timing) != sNotStretches.end();
            });
            constexpr std::size_t shown = 3;
            std::partial_sort(spends.begin(), spends.begin() + shown, end,
                [&](const Timing a, const Timing b) { return frame.mSpend.at(a) > frame.mSpend.at(b); });

            std::string described;
            for (std::size_t at = 0; at < shown; ++at)
                described += std::format(" {} {:.1f}", sTimings.name(spends[at]), frame.mSpend.at(spends[at]));

            return std::format("{:.1f} ms ({} meshes:{})", frame.mFrameMs, frame.mArrivedMeshes, described);
        }

        std::string asJson(const Crossings& crossings)
        {
            return std::format(R"({{"count": {}, "rebuilds": {}, "worstMs": {:.2f}, "totalMs": {:.2f}}})",
                crossings.mCount, crossings.mRebuilds, crossings.mWorstMs, crossings.mTotalMs);
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

    void Arrivals::add(const double frameMs, const std::uint32_t arrivedMeshes, const FrameSpend& spend)
    {
        if (arrivedMeshes > 0)
        {
            ++mFrames;
            mMeshes += arrivedMeshes;
            mSumMs += frameMs;
            mWorstMs = std::max(mWorstMs, frameMs);
        }

        // Kept longest first, so the shortest kept is the one a longer frame pushes out.
        std::size_t at = mWorstCount;
        while (at > 0 && mWorst[at - 1].mFrameMs < frameMs)
            --at;
        if (at >= sKept)
            return;

        for (std::size_t behind = std::min(mWorstCount, sKept - 1); behind > at; --behind)
            mWorst[behind] = mWorst[behind - 1];
        mWorst[at] = WorstFrame{ .mFrameMs = frameMs, .mArrivedMeshes = arrivedMeshes, .mSpend = spend };
        mWorstCount = std::min(mWorstCount + 1, sKept);
    }

    int minuteOfDay(const float hour)
    {
        return static_cast<int>(std::lround(hour * 60.0f)) % (24 * 60);
    }

    std::string describeHour(const float hour)
    {
        const int minutes = minuteOfDay(hour);
        return std::format("{:02}:{:02}", minutes / 60, minutes % 60);
    }

    namespace
    {
        /// What compaction has left to give back, or nothing where there is none and where the
        /// device would not say.
        ///
        /// **Left out rather than printed as nought**, because a pair reading "would compact to 0.0"
        /// is a saving of everything rather than an answer nobody has — and a pair reading "52.5 of
        /// them would compact to 52.5" is a settled cell saying so at length.
        std::string describeCompaction(const SceneStats& scene)
        {
            if (scene.mCompactableBytes == 0 || scene.mCompactableNowBytes <= scene.mCompactableBytes)
                return {};

            return std::format(" ({:.1f} of them would compact to {:.1f})", megabytes(scene.mCompactableNowBytes),
                megabytes(scene.mCompactableBytes));
        }
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
                "  cell {} at {} in {}   {} instances ({} cutouts)   {:.1f} MiB structures in "
                "{:.1f} reserved{}   {} textures, {:.1f} MiB\n",
                place.mCell, describeHour(place.mHour), place.mWeather, place.mScene.mInstances.mPlaced,
                place.mScene.mInstances.mCutout, megabytes(place.mScene.mStructureLiveBytes),
                megabytes(place.mScene.mStructureBytes), describeCompaction(place.mScene), place.mScene.mTextureCount,
                megabytes(place.mScene.mTextureBytes));

        // **Two facts and not one line.** A staged place pays one build before its frames and can
        // name what it cost; a run of a real game builds a little at every crossing and has no such
        // number. What both have is how much of the frame hit something, which is what tells "the
        // cell rendered" from "the camera faced away from it".
        // **Under the scene's line, because it is the same fact from the device's side.** The line
        // above says what the content came to; this says what the card gave up for it, and the
        // second is the one a card with a small host-visible heap runs out of first.
        out += describeMemory(place.mMemory);

        if (place.mHitPercent > 0.0)
            out += std::format("  {:.1f}% of primary rays hit\n", place.mHitPercent);

        if (place.mOverlap.mFrames > 0)
            out += std::format("  {:.2f} frames in flight at a submit, {} at the least\n", place.mOverlap.getMean(),
                place.mOverlap.mLeast);

        out += describeHeadings();
        for (const Timing timing : sTimings.values())
            out += describeTimes(std::format("{} ms", sTimings.name(timing)), place.mRows[indexOf(timing)]);

        // The driver's figure under the host's rows, in the same columns: the one number a player
        // feels, from the only party that sees the whole of the pipeline. Only where the driver
        // paced the window.
        if (place.mLatency.has_value())
            out += describeTimes("latency ms", *place.mLatency);

        // **The device's own account of the same frame, one figure each.** Six distributions would
        // be a wall; what this row answers is "which of them is the expensive one", and the row
        // above already says how much the whole frame varies. Each figure is the zone's share of
        // the average frame, so the row sums to the device's part of it and a pass that only runs
        // at a crossing says so beside its own share.
        out += describeZones(place.mGpu);
        out += describeClock(place.mClock);

        // Under the clock, because it is the other premise every figure above rests on: a place
        // another process drew through is the desktop's reading and not the renderer's.
        out += "  " + describeCard(place.mCard) + '\n';

        // Only where the run watched: a window somebody is looking at watches nothing.
        if (place.mThreads.mViewed)
            out += "  " + describeThreads(place.mThreads) + '\n';

        // **Only for a route, because a place that stands still has nothing to say here.** The
        // worst is the one to read: a crossing is a dropped frame, and an average over six hundred
        // frames of which four were the expensive ones hides exactly the thing.
        if (place.mCrossings.mCount > 0)
            out += std::format("  {} crossings, {} of them rebuilds — {:.0f} ms worst, {:.1f} s over the run{}\n",
                place.mCrossings.mCount, place.mCrossings.mRebuilds, place.mCrossings.mWorstMs,
                place.mCrossings.mTotalMs / 1000.0,
                place.mTravelled < 1.0 ? std::format(", {:.0f}% of the route flown", place.mTravelled * 100.0) : "");

        // **The arrivals beside the crossings, at every place.** A crossing is a route's; a mesh
        // arrives wherever an actor walks in, and what the worst frames carried is the one reading
        // that says whether the tail is theirs.
        if (place.mArrivals.mFrames > 0)
            out += std::format(
                "  {} frames extended the scene with {} meshes — {:.1f} ms worst, {:.1f} mean against "
                "the {:.1f} median\n",
                place.mArrivals.mFrames, place.mArrivals.mMeshes, place.mArrivals.mWorstMs, place.mArrivals.getMeanMs(),
                place.at(Timing::Frame).mMedian);
        if (place.mArrivals.mWorstCount > 0)
        {
            out += "  worst frames:";
            for (std::size_t at = 0; at < place.mArrivals.mWorstCount; ++at)
                out += std::format("{} {}", at == 0 ? "" : ",", describeWorst(place.mArrivals.mWorst[at]));
            out += '\n';
        }

        out += std::format("  {} frames in {:.2f} s — {:.1f} fps, {:.1f} at the 1% low\n", place.mFrames,
            place.mWallSeconds, place.at(Timing::Frame).getRate(), place.at(Timing::Frame).getLowRate());

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
             << std::format(R"(  "upscale": "{}",)", sUpscaleNames.name(header.mUpscaling.mMode)) << '\n'
             << std::format(R"(  "preset": "{}",)", sPresetNames.name(header.mUpscaling.mPreset)) << '\n'
             << std::format(R"(  "noise": "{}", "levelBias": {:.3f}, "reorder": "{}",)",
                    sNoiseSourceNames.name(header.mNoise), header.mLevelBias, sReorderNames.name(header.mReorder))
             << '\n'
             << std::format(R"(  "frames": {}, "warmup": {}, "validation": {},)", header.mMeasured, header.mWarmup,
                    header.mValidating)
             << '\n'
             << R"(  "places": [)" << '\n';

        for (std::size_t at = 0; at < places.size(); ++at)
        {
            const BenchPlace& place = places[at];
            file << std::format(R"(    {{"view": "{}", "cell": "{}", "hour": {}, "weather": "{}", )", place.mView,
                place.mCell, place.mHour, place.mWeather)
                 << R"("scene": )" << asJson(place.mScene)
                 << std::format(R"(, "frames": {}, "wallSeconds": {:.4f}, "hitPercent": {:.2f}, )", place.mFrames,
                        place.mWallSeconds, place.mHitPercent)
                 << R"("crossings": )" << asJson(place.mCrossings) << R"(, "arrivals": )" << asJson(place.mArrivals)
                 << std::format(R"(, "overlap": {{"mean": {:.4f}, "least": {}}}, "travelled": {:.4f}, )",
                        place.mOverlap.getMean(), place.mOverlap.mLeast, place.mTravelled);

            if (place.mLatency.has_value())
                file << R"("latencyMs": )" << asJson(*place.mLatency) << ", ";
            for (const Timing timing : sTimings.values())
                file << std::format(R"("{}Ms": )", sTimings.name(timing)) << asJson(place.mRows[indexOf(timing)])
                     << ", ";

            file << R"("gpuMs": {)";

            for (std::size_t zone = 0; zone < place.mGpu.size(); ++zone)
                file << std::format(
                    R"({}"{}": {})", zone == 0 ? "" : ", ", place.mGpu[zone].mName, asJson(place.mGpu[zone]));

            file << "}, \"clock\": " << asJson(place.mClock) << ", \"card\": " << asJson(place.mCard) << "}"
                 << (at + 1 < places.size() ? "," : "") << '\n';
        }

        file << "  ]\n}\n";
    }
}
