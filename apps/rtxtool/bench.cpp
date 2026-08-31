#include "bench.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include <SDL.h>

#include <components/debug/debugging.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/files/conversion.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/sceneuploader.hpp>
#include <components/rtx/wavespectrum.hpp>
#include <components/sky/clouds.hpp>
#include <components/sky/skyroll.hpp>

#include "content.hpp"
#include "framehashes.hpp"
#include "framing.hpp"
#include "perfcontrol.hpp"
#include "stagedworld.hpp"
#include "viewpoint.hpp"
#include "window.hpp"
#include "world.hpp"

namespace RtxTool
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        std::ostream& out()
        {
            return Debug::getRawStdout();
        }

        double megabytes(std::uint64_t bytes)
        {
            return static_cast<double>(bytes) / (1024.0 * 1024.0);
        }

        void report(const BenchPlace& place)
        {
            out() << '\n' << place.mView;
            if (!place.mNote.empty())
                out() << " — " << place.mNote;

            out() << '\n'
                  << std::format(
                         "  cell {} at {}   {} instances ({} cutouts)   {:.1f} MiB structures   {} textures, "
                         "{:.1f} MiB\n",
                         place.mCell, clockFace(place.mHour), place.mScene.mInstances, place.mScene.mCutoutInstances,
                         megabytes(place.mScene.mStructureBytes), place.mScene.mTextureCount,
                         megabytes(place.mScene.mTextureBytes))
                  << std::format("  build {:.0f} ms   {:.1f}% of primary rays hit\n", place.mBuildMs, place.mHitPercent)
                  << Rtx::describeHeadings() << Rtx::describeTimes("frame ms", place.mFrame)
                  << Rtx::describeTimes("wait ms", place.mWait) << Rtx::describeTimes("walk ms", place.mWalk)
                  << Rtx::describeTimes("place ms", place.mPlace);

            // **The device's own account of the same frame, medians only.** Six distributions would
            // be a wall; what this row answers is "which of them is the expensive one", and the row
            // above already says how much the whole frame varies.
            out() << Rtx::describeZones(place.mGpu) << describeClock(place.mClock);

            // **Only for a route, because a place that stands still has nothing to say here.** The
            // worst is the one to read: a crossing is a dropped frame, and an average over six
            // hundred frames of which four were the expensive ones hides exactly the thing.
            if (place.mCrossings.mCount > 0)
                out() << std::format(
                    "  {} crossings, {} of them rebuilds — {:.0f} ms worst; {:.1f} s over the run, "
                    "{:.1f} reading and {:.1f} building{}\n",
                    place.mCrossings.mCount, place.mCrossings.mRebuilds, place.mCrossings.mWorstMs,
                    (place.mCrossings.mReadMs + place.mCrossings.mBuildMs) / 1000.0, place.mCrossings.mReadMs / 1000.0,
                    place.mCrossings.mBuildMs / 1000.0,
                    place.mTravelled < 1.0 ? std::format(", {:.0f}% of the route flown", place.mTravelled * 100.0)
                                           : "");

            out() << std::format("  {} frames in {:.2f} s — {:.1f} fps, {:.1f} at the 1% low\n", place.mFrames,
                place.mWallSeconds, place.mFrame.getRate(), place.mFrame.getLowRate());
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
        std::string asJson(const Rtx::SceneStats& scene)
        {
            return std::format(R"({{"instances": {}, "cutoutInstances": {}, "micromappedInstances": {}, )"
                               R"("micromapOpaque": {:.6f}, "micromapTransparent": {:.6f}, "micromapUnknown": {:.6f}, )"
                               R"("structureBytes": {}, "tableBytes": {}, "textureCount": {}, "textureBytes": {}}})",
                scene.mInstances, scene.mCutoutInstances, scene.mMicromappedInstances, scene.mMicromapTally.mOpaque,
                scene.mMicromapTally.mTransparent, scene.mMicromapTally.mUnknown, scene.mStructureBytes,
                scene.mTableBytes, scene.mTextureCount, scene.mTextureBytes);
        }

        std::string asJson(const Crossings& crossings)
        {
            return std::format(
                R"({{"count": {}, "rebuilds": {}, "worstMs": {:.2f}, "readMs": {:.2f}, "buildMs": {:.2f}}})",
                crossings.mCount, crossings.mRebuilds, crossings.mWorstMs, crossings.mReadMs, crossings.mBuildMs);
        }

        std::string asJson(const Rtx::FrameTimes& times)
        {
            return std::format(
                R"({{"median": {:.4f}, "mean": {:.4f}, "p95": {:.4f}, "p99": {:.4f}, "best": {:.4f}, "worst": {:.4f}}})",
                times.mMedian, times.mMean, times.mP95, times.mP99, times.mBest, times.mWorst);
        }

        /// Writes the run as one record, for comparing against the same run on another commit.
        ///
        /// Hand-written rather than through a library: this is numbers and the names of places, and
        /// the alternative is a dependency for the sake of a page. Whatever is a record of its own —
        /// a scene, a distribution, a clock — has an `asJson` above, so a field added to one of
        /// those reaches this without anybody remembering to come here.
        void writeJson(const std::filesystem::path& path, const BenchRequest& request, const Rtx::FrameExtents& extents,
            bool validating, const std::vector<BenchPlace>& places)
        {
            std::ofstream file(path);

            file << "{\n"
                 << std::format(R"(  "suite": "{}",)", request.mSuite) << '\n'
                 << std::format(R"(  "output": [{}, {}],)", extents.mOutputWidth, extents.mOutputHeight) << '\n'
                 << std::format(R"(  "render": [{}, {}],)", extents.mRenderWidth, extents.mRenderHeight) << '\n'
                 << std::format(R"(  "upscale": "{}",)", Rtx::upscaleName(request.mFrame.mUpscale)) << '\n'
                 << std::format(R"(  "preset": "{}",)", Rtx::presetName(request.mFrame.mPreset)) << '\n'
                 << std::format(R"(  "frames": {}, "warmup": {}, "validation": {},)", request.getMeasured(),
                        request.getWarmup(), validating)
                 << '\n'
                 << R"(  "places": [)" << '\n';

            for (std::size_t at = 0; at < places.size(); ++at)
            {
                const BenchPlace& place = places[at];
                file << std::format(R"(    {{"view": "{}", "cell": "{}", "hour": {}, "buildMs": {:.2f}, )", place.mView,
                    place.mCell, place.mHour, place.mBuildMs)
                     << R"("scene": )" << asJson(place.mScene)
                     << std::format(R"(, "frames": {}, "wallSeconds": {:.4f}, "hitPercent": {:.2f}, )", place.mFrames,
                            place.mWallSeconds, place.mHitPercent)
                     << R"("crossings": )" << asJson(place.mCrossings)
                     << std::format(R"(, "travelled": {:.4f}, )", place.mTravelled) << R"("frameMs": )"
                     << asJson(place.mFrame) << R"(, "waitMs": )" << asJson(place.mWait) << R"(, "walkMs": )"
                     << asJson(place.mWalk) << R"(, "placeMs": )" << asJson(place.mPlace) << R"(, "gpuMs": {)";

                for (std::size_t zone = 0; zone < place.mGpu.size(); ++zone)
                    file << std::format(R"({}"{}": {})", zone == 0 ? "" : ", ", place.mGpu[zone].mName,
                        asJson(place.mGpu[zone].mTimes));

                file << "}, \"clock\": " << asJson(place.mClock) << "}" << (at + 1 < places.size() ? "," : "") << '\n';
            }

            file << "  ]\n}\n";
        }

        /// Whether someone has asked for the run to stop. Events are pumped whether or not there is
        /// a window: SDL is initialised either way, and a window nobody drains stops being drawn by
        /// the compositor and starts being reported as hung.
        bool interrupted()
        {
            bool stop = false;
            SDL_Event event;
            while (SDL_PollEvent(&event) != 0)
            {
                if (event.type == SDL_QUIT || (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE))
                    stop = true;
            }

            return stop;
        }
    }

    std::uint32_t BenchRequest::getMeasured() const
    {
        if (mFrames > 0)
            return mFrames;

        return std::max(1u, static_cast<std::uint32_t>(std::lround(mSeconds * sStepRate)));
    }

    std::uint32_t BenchRequest::getWarmup() const
    {
        return static_cast<std::uint32_t>(std::lround(std::max(mWarmup, 0.0f) * sStepRate));
    }

    /// Everything one place is measured with, which is the same for every place in a run.
    struct BenchRun
    {
        World& mWorld;
        Rtx::Renderer& mRenderer;
        const BenchRequest& mRequest;
        PerfControl& mProfiling;

        /// Where the run is shown, or null where nobody asked to watch it.
        Window* mWindow = nullptr;

        /// Whether every frame is read back and hashed, which stops the run being a benchmark:
        /// a read back submits a copy and waits on it, so every frame is serialised against the
        /// device and what the rows measure is that.
        bool mJudging = false;
        FrameHashes& mHashes;

        std::uint32_t mWarmup = 0;
        std::uint32_t mMeasured = 0;
    };

    /// Runs one place and gives back what it came to. Nothing where the run was interrupted before
    /// a single frame had been measured.
    ///
    /// **The whole of the measuring, and none of the choosing or the reporting.** Which places there
    /// are, whether each could be staged at all, and what is done with the rows are `runBench`'s;
    /// what a place costs is this.
    ///
    /// @param samples and `pixelScratch` belong to the run and are refilled here, so that a place
    ///        does not allocate what the place before it already had.
    std::optional<BenchPlace> measurePlace(const BenchRun& run, const View& view, StagedWorld& staged,
        Rtx::FrameSamples& samples, std::vector<std::uint8_t>& pixelScratch, bool& stopped)
    {
        Rtx::Renderer& renderer = run.mRenderer;

        Rtx::SceneUploader uploader;

        // **A hashed run takes its composites on the schedule and not off the baker's clock**,
        // which is what `SceneUploader::setSettled` is for. It stalls at the crossing that
        // queued them, and a run being hashed has already given up on its own times.
        uploader.setSettled(run.mJudging);

        const Clock::time_point buildStart = Clock::now();
        uploader.hand(renderer, Rtx::sWorld, staged.getScene(), run.mWorld.getImageManager(), Rtx::SeaState{});
        const double buildMs = std::chrono::duration<double, std::milli>(Clock::now() - buildStart).count();

        const float far = std::max(staged.getScene().getBounds().radius() * 8.0f, 10000.0f);

        samples.clear();

        Crossings crossings;
        float part = 0.0f;

        // Per place, because the zones a place has are the zones its content asked for: an
        // interior with nothing moving in it never places and never reports one.
        Rtx::GpuBreakdown gpu;

        // Both ends of the measured window, so what it reports bounds the frames between them.
        GpuClock clock;

        std::uint32_t hits = 0;

        // Restarted when the warmup ends, so `mWallSeconds` covers the frames `mFrames`
        // counts. A clock left running from here would divide six hundred frames by the time
        // six hundred and sixty took, and the sixty are the slow ones.
        Clock::time_point runStart = Clock::now();

        for (std::uint32_t frame = 0; frame < run.mWarmup + run.mMeasured; ++frame)
        {
            // Pumped outside the timing: SDL is what keeps a window being drawn rather than
            // reported as hung, and it is no part of what the renderer costs.
            if (interrupted())
            {
                stopped = true;
                break;
            }

            if (frame == run.mWarmup)
            {
                // **Where the measured frames begin, and again where they end.** One reading is
                // one moment: taken only at the end it is a card already climbing off the load,
                // and it would print a fast clock over frames drawn at a slower one. Outside
                // the wall clock and before `frameStart`, so the process spawn it costs is in
                // no frame's time.
                clock.add(readGpuClock());

                runStart = Clock::now();
                run.mProfiling.enable();
            }

            const Clock::time_point frameStart = Clock::now();

            // **The route runs over the measured frames and not the warm-up.** Warming up is
            // the GPU coming off its idle clock; flying during it would start the measurement
            // partway along and leave the first crossing outside the numbers.
            //
            // **And off the frame index rather than the clock**, for the reason the world is
            // stepped that way: a camera advanced by how long the last frame took crosses its
            // boundaries somewhere else on every machine, and where they fall is the whole
            // measurement.
            Placement standing = staged.getPlacement();
            if (view.mRoute.has_value() && frame >= run.mWarmup)
            {
                part = view.mRoute->partAt(staged.getPlacement(), static_cast<float>(frame - run.mWarmup) / sStepRate);
                standing = view.mRoute->at(staged.getPlacement(), part);
            }

            // **Inside the frame's own timing, because a crossing is a dropped frame.** Timing
            // it outside would report a smooth run with a load cost printed beside it, which is
            // the opposite of what a p99 is for.
            if (const Crossing crossed = staged.moveTo(standing.mOrigin); crossed.happened())
            {
                const Clock::time_point read = Clock::now();
                const Rtx::SceneUpload handed = uploader.hand(
                    renderer, Rtx::sWorld, staged.getScene(), run.mWorld.getImageManager(), Rtx::SeaState{});
                const Clock::time_point built = Clock::now();

                crossings.add(handed.mKind == Rtx::SceneUpload::Kind::Rebuilt,
                    std::chrono::duration<double, std::milli>(read - frameStart).count(),
                    std::chrono::duration<double, std::milli>(built - read).count());
            }

            // **After the first, which is the frame the build above already made**, and by
            // frame index rather than by the clock: a world stepped by how long the last frame
            // took would render a different sequence on every machine and on every build.
            double walkMs = 0.0;
            bool moved = false;
            if (frame > 0 && staged.getMotion() != nullptr)
            {
                const Clock::time_point walkStart = Clock::now();
                moved = staged.getMotion()->step(frame);
                walkMs = std::chrono::duration<double, std::milli>(Clock::now() - walkStart).count();
            }

            // **Between the walk and the placement, which is where the wait belongs.** The walk
            // runs beside the device drawing the frame behind; the placement cannot, because it
            // writes the copy of the tables that frame is still tracing and `placeScene` waits
            // that frame out first. Waited for here, the stall is one figure and it is in
            // `wait ms`; left to the placement it is inside `place ms` as well, and that row
            // then reads as placement work — 8.3 ms at 3840x2160 against 1.8 at 1920x1080, for
            // the same rows written.
            //
            // **Before the submit below, which is what makes this the frame behind** rather than
            // the one about to be made — `Renderer::finishFrame` says why. So the rows below
            // report a frame one older than the wall time beside them, and both are medians
            // over the run.
            const std::optional<Rtx::FrameResult> result = renderer.finishFrame();

            // Handed rather than placed because a step walks the whole graph and sweeps it: an
            // actor drawing a weapon brings a mesh nothing has built, and a sweep that closed a
            // gap renumbers what the last frame was built from.
            double placeMs = 0.0;
            if (moved)
            {
                const Clock::time_point placeStart = Clock::now();
                uploader.hand(renderer, Rtx::sWorld, staged.getScene(), run.mWorld.getImageManager(), Rtx::SeaState{});
                placeMs = std::chrono::duration<double, std::milli>(Clock::now() - placeStart).count();
            }

            Framing framing = Framing::lookingFrom(standing);
            framing.mFieldOfView = run.mRequest.mFrame.mFieldOfView;
            framing.mFar = far;
            framing.mDelight = run.mRequest.mFrame.mDelight;

            framing.mLighting = staged.getLighting();
            framing.mLighting.mSeconds = static_cast<float>(frame) / sStepRate;

            // **Off the frame index, like everything else a measured run animates.** The hour
            // does not move here, so no game time passes for the star sphere to turn on, and
            // the deck scrolls on the player's clock — which is the one this index counts.
            framing.mLighting.mRoll = Sky::SkyRoll::after(
                framing.mLighting.mSeconds, framing.mLighting.mCloudSpeed, 0.0f, Sky::timescaleClouds());

            // What the upscaler's sample sequence and every random draw in the shader are walked
            // by. Held to the frame index so the same run draws the same samples twice over.
            framing.mFrame = frame;

            renderer.renderFrame(makeFrameConstants(framing, renderer.getExtents()),
                Rtx::FrameOptions{ .mSinceLast = sStepSeconds,
                    .mExposureBias = framing.mLighting.mDaylight.mExposureBias,
                    .mFilter = run.mRequest.mFrame.mFilter,
                    .mExposure = run.mRequest.mFrame.mExposure });

            if (run.mWindow != nullptr && !renderer.presentFrame())
                renderer.resize(run.mWindow->getWidth(), run.mWindow->getHeight());

            if (run.mJudging)
            {
                renderer.readPixels(pixelScratch);
                run.mHashes.add(view.mName, frame, pixelScratch);
            }

            const double frameMs = std::chrono::duration<double, std::milli>(Clock::now() - frameStart).count();

            if (frame >= run.mWarmup)
            {
                samples.add(frameMs, walkMs, placeMs);
                if (result.has_value())
                {
                    samples.addWait(result->mWaitMs);
                    gpu.add(result->mGpu);
                    hits = result->mHits;
                }
            }
        }

        // The last frame is still on the device when the loop ends, and its row is owed.
        for (std::optional<Rtx::FrameResult> last = renderer.finishFrame(); last.has_value();
             last = renderer.finishFrame())
        {
            if (samples.empty())
                continue;

            samples.addWait(last->mWaitMs);
            gpu.add(last->mGpu);
            hits = last->mHits;
        }

        const Clock::time_point runEnd = Clock::now();
        run.mProfiling.disable();

        // After the wall clock above and not before it, so the spawn this costs is outside the
        // run it describes.
        clock.add(readGpuClock());

        if (samples.empty())
            return std::nullopt;

        const Rtx::FrameExtents traced = renderer.getExtents();
        const double pixels = static_cast<double>(traced.mRenderWidth) * traced.mRenderHeight;

        // Once: summarising sorts the rows in place, so a second call would be re-sorting what
        // the first one's iterators point at.
        const std::span<const Rtx::GpuZone> zones = gpu.summariseZones();

        return BenchPlace{
            .mView = view.mName,
            .mCell = view.mCell,
            .mNote = view.mNote,
            .mHour = view.mHour.value_or(run.mRequest.mFrame.mHour),
            .mBuildMs = buildMs,
            .mFrames = samples.size(),
            .mWallSeconds = std::chrono::duration<double>(runEnd - runStart).count(),
            .mFrame = Rtx::summarise(samples.mFrame),
            .mWait = Rtx::summarise(samples.mWait),
            .mWalk = Rtx::summarise(samples.mWalk),
            .mPlace = Rtx::summarise(samples.mPlace),
            .mGpu = std::vector<Rtx::GpuZone>(zones.begin(), zones.end()),
            .mClock = clock,
            .mHitPercent = static_cast<double>(hits) / pixels * 100.0,
            .mCrossings = crossings,
            .mTravelled = view.mRoute.has_value() ? static_cast<double>(part) : 1.0,
            .mScene = renderer.getSceneStats(),
        };
    }

    int runBench(World& world, const Rtx::ValidationOptions& validation, const BenchRequest& request)
    {
        const std::uint32_t measured = request.getMeasured();
        const std::uint32_t warmup = request.getWarmup();

        PerfControl profiling(request.mPerfControl);

        std::unique_ptr<Window> window;
        if (request.mWindow)
            window = std::make_unique<Window>("OpenMW RTX - bench", request.mFrame.mWidth, request.mFrame.mHeight);

        std::string reason;
        // **One renderer for the whole run.** Standing one up compiles every pipeline and costs a
        // quarter of a second; doing that per place would put a cold device in front of every
        // measurement and make the first place in the list systematically the slowest.
        const std::unique_ptr<Rtx::Renderer> renderer = Rtx::createRenderer(
            request.mFrame.describeRenderer(validation, window == nullptr ? nullptr : window->getHandle()), reason);
        if (renderer == nullptr)
        {
            out() << reason << '\n';
            return 1;
        }

        const Rtx::FrameExtents extents = renderer->getExtents();

        out() << std::format("bench: {} {}, {} frames each ({:.1f} s of world at {:.0f} Hz) after {} warming up\n",
            request.mViews.size(), request.mViews.size() == 1 ? "place" : "places", measured,
            static_cast<double>(measured) / static_cast<double>(sStepRate), sStepRate, warmup);

        out() << std::format("       {}x{}", extents.mOutputWidth, extents.mOutputHeight);
        if (extents.mRenderWidth != extents.mOutputWidth || extents.mRenderHeight != extents.mOutputHeight)
            out() << std::format(" traced at {}x{}", extents.mRenderWidth, extents.mRenderHeight);

        out() << std::format(", upscale {}, preset {}", Rtx::upscaleName(request.mFrame.mUpscale),
            Rtx::presetName(request.mFrame.mPreset));

        // **Said before the run rather than after it.** A figure measured under the layers is not
        // one to compare against anything, and finding that out at the end is finding it out after
        // the ten minutes have been spent.
        if (renderer->isValidating())
            out() << ", WITH THE VALIDATION LAYERS ON — pass --validation=false for a number worth quoting";

        out() << "\n";

        // **A hashed run is not a timed one**, and it says so where the times are read rather than
        // only in `--help`: reading a frame back submits a copy and waits on it, so every frame is
        // serialised against the device and the rows below measure that.
        const bool judging = !request.mHashes.empty() || !request.mAgainst.empty();
        if (judging)
            out() << "       hashing every frame, so the times below are not a benchmark\n";

        FrameHashes hashes;
        const FrameHashes reference = request.mAgainst.empty() ? FrameHashes{} : FrameHashes::read(request.mAgainst);
        // Cleared and refilled by every frame that is hashed, never freed.
        std::vector<std::uint8_t> pixelScratch;

        std::vector<BenchPlace> places;
        places.reserve(request.mViews.size());

        Rtx::FrameSamples samples;
        samples.reserve(measured);

        bool stopped = false;

        const BenchRun run{
            .mWorld = world,
            .mRenderer = *renderer,
            .mRequest = request,
            .mProfiling = profiling,
            .mWindow = window.get(),
            .mJudging = judging,
            .mHashes = hashes,
            .mWarmup = warmup,
            .mMeasured = measured,
        };

        for (const View& view : request.mViews)
        {
            const ESM::Cell* cell = world.getContent().findCell(view.mCell);
            if (cell == nullptr)
            {
                out() << "\n" << view.mName << ": no cell called \"" << view.mCell << "\"\n";
                return 1;
            }

            if (window != nullptr)
                window->setTitle("OpenMW RTX - bench - " + view.mName);

            StagedWorld staged(world, *cell, request.mFrame.describeStaging(view.mHour, view.mOrigin, view.mTarget),
                request.mFrame.mActors);

            if (staged.empty())
            {
                out() << "\n" << view.mName << ": the region placed no geometry\n";
                return 1;
            }

            const std::optional<BenchPlace> place = measurePlace(run, view, staged, samples, pixelScratch, stopped);

            if (!place.has_value())
                break;

            places.push_back(*place);
            report(places.back());
        }

        if (places.empty())
        {
            out() << "\nstopped before anything was measured\n";
            return 1;
        }

        if (!request.mHashes.empty())
        {
            hashes.write(request.mHashes);
            out() << std::format(
                "\nwrote {} frame hashes to {}\n", hashes.frameCount(), Files::pathToUnicodeString(request.mHashes));
        }

        int judgement = 0;
        if (!request.mAgainst.empty())
        {
            out() << std::format("\nagainst {}\n", Files::pathToUnicodeString(request.mAgainst));
            for (const FrameHashes::ViewDifference& difference : hashes.against(reference))
            {
                out() << std::format("  {:<28} {}\n", difference.mView, describe(difference));
                if (!difference.same())
                    judgement = 1;
            }
        }

        std::uint32_t frames = 0;
        double lasted = 0.0;
        for (const BenchPlace& place : places)
        {
            frames += place.mFrames;
            lasted += place.mWallSeconds;
        }

        out() << std::format("\n{} {}, {} frames in {:.1f} s{}\n", places.size(),
            places.size() == 1 ? "place" : "places", frames, lasted, stopped ? " — stopped early" : "");

        if (!request.mJson.empty())
        {
            writeJson(request.mJson, request, extents, renderer->isValidating(), places);
            out() << "wrote " << Files::pathToUnicodeString(request.mJson) << '\n';
        }

        return judgement;
    }
}
