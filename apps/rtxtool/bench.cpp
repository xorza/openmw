#include "bench.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <fstream>
#include <memory>
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

#include "framing.hpp"
#include "perfcontrol.hpp"
#include "stagedworld.hpp"
#include "window.hpp"
#include "world.hpp"

namespace RtxTool
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        /// What the world advances per frame, which is `PosedActors`' own step and not a second
        /// opinion about it: an actor has to move the same amount per frame here as it does in the
        /// game, and two clocks that disagreed would make a run irreproducible in the one way that
        /// matters.
        constexpr float sStepRate = 60.0f;

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
                         "  cell {}   {} instances ({} cutouts)   {:.1f} MiB structures   {} textures, "
                         "{:.1f} MiB\n",
                         place.mCell, place.mScene.mInstances, place.mScene.mCutoutInstances,
                         megabytes(place.mScene.mStructureBytes), place.mScene.mTextureCount,
                         megabytes(place.mScene.mTextureBytes))
                  << std::format("  build {:.0f} ms   {:.1f}% of primary rays hit\n", place.mBuildMs, place.mHitPercent)
                  << Rtx::describeHeadings() << Rtx::describeTimes("frame ms", place.mFrame)
                  << Rtx::describeTimes("trace ms", place.mTrace) << Rtx::describeTimes("place ms", place.mPlace);

            // **The device's own account of the same frame, medians only.** Six distributions would
            // be a wall; what this row answers is "which of them is the expensive one", and the row
            // above already says how much the whole frame varies.
            out() << Rtx::describeZones(place.mGpu);

            // **Only for a route, because a place that stands still has nothing to say here.** The
            // worst is the one to read: a crossing is a dropped frame, and an average over six
            // hundred frames of which four were the expensive ones hides exactly the thing.
            if (place.mCrossings > 0)
                out() << std::format(
                    "  {} crossings, {} of them rebuilds — {:.0f} ms worst; {:.1f} s over the run, "
                    "{:.1f} reading and {:.1f} building{}\n",
                    place.mCrossings, place.mCrossRebuilds, place.mCrossWorstMs,
                    (place.mCrossReadMs + place.mCrossBuildMs) / 1000.0, place.mCrossReadMs / 1000.0,
                    place.mCrossBuildMs / 1000.0,
                    place.mTravelled < 1.0 ? std::format(", {:.0f}% of the route flown", place.mTravelled * 100.0)
                                           : "");

            out() << std::format("  {} frames in {:.2f} s — {:.1f} fps, {:.1f} at the 1% low\n", place.mFrames,
                place.mWallSeconds, place.mFrame.getRate(), place.mFrame.getLowRate());
        }

        std::string asJson(const Rtx::FrameTimes& times)
        {
            return std::format(
                R"({{"median": {:.4f}, "mean": {:.4f}, "p95": {:.4f}, "p99": {:.4f}, "best": {:.4f}, "worst": {:.4f}}})",
                times.mMedian, times.mMean, times.mP95, times.mP99, times.mBest, times.mWorst);
        }

        /// Writes the run as one record, for comparing against the same run on another commit.
        ///
        /// Hand-written rather than through a library: this is a flat object of numbers, and the
        /// alternative is a dependency for the sake of six lines.
        void writeJson(const std::filesystem::path& path, const BenchRequest& request, const Rtx::FrameExtents& extents,
            bool validating, const std::vector<BenchPlace>& places)
        {
            std::ofstream file(path);

            file << "{\n"
                 << std::format(R"(  "suite": "{}",)", request.mSuite) << '\n'
                 << std::format(R"(  "output": [{}, {}],)", extents.mOutputWidth, extents.mOutputHeight) << '\n'
                 << std::format(R"(  "render": [{}, {}],)", extents.mRenderWidth, extents.mRenderHeight) << '\n'
                 << std::format(R"(  "upscale": "{}",)", Rtx::upscaleName(request.mUpscale)) << '\n'
                 << std::format(R"(  "preset": "{}",)", Rtx::presetName(request.mPreset)) << '\n'
                 << std::format(R"(  "frames": {}, "warmup": {}, "validation": {},)", request.getMeasured(),
                        request.getWarmup(), validating)
                 << '\n'
                 << R"(  "places": [)" << '\n';

            for (std::size_t at = 0; at < places.size(); ++at)
            {
                const BenchPlace& place = places[at];
                file << std::format(R"(    {{"view": "{}", "cell": "{}", "instances": {}, "buildMs": {:.2f}, )",
                    place.mView, place.mCell, place.mScene.mInstances, place.mBuildMs)
                     << std::format(R"("frames": {}, "wallSeconds": {:.4f}, "hitPercent": {:.2f}, )", place.mFrames,
                            place.mWallSeconds, place.mHitPercent)
                     << std::format(
                            R"("crossings": {}, "crossRebuilds": {}, "crossWorstMs": {:.2f}, "crossReadMs": {:.2f}, )"
                            R"("crossBuildMs": {:.2f}, "travelled": {:.4f}, )",
                            place.mCrossings, place.mCrossRebuilds, place.mCrossWorstMs, place.mCrossReadMs,
                            place.mCrossBuildMs, place.mTravelled)
                     << R"("frameMs": )" << asJson(place.mFrame) << R"(, "traceMs": )" << asJson(place.mTrace)
                     << R"(, "placeMs": )" << asJson(place.mPlace) << R"(, "gpuMs": {)";

                for (std::size_t zone = 0; zone < place.mGpu.size(); ++zone)
                    file << std::format(R"({}"{}": {})", zone == 0 ? "" : ", ", place.mGpu[zone].mName,
                        asJson(place.mGpu[zone].mTimes));

                file << "}}" << (at + 1 < places.size() ? "," : "") << '\n';
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

    int runBench(World& world, const Rtx::ValidationOptions& validation, const BenchRequest& request)
    {
        const std::uint32_t measured = request.getMeasured();
        const std::uint32_t warmup = request.getWarmup();

        PerfControl profiling(request.mPerfControl);

        std::unique_ptr<Window> window;
        if (request.mWindow)
            window = std::make_unique<Window>("OpenMW RTX - bench", request.mWidth, request.mHeight);

        std::string reason;
        // **One renderer for the whole run.** Standing one up compiles every pipeline and costs a
        // quarter of a second; doing that per place would put a cold device in front of every
        // measurement and make the first place in the list systematically the slowest.
        const std::unique_ptr<Rtx::Renderer> renderer = Rtx::createRenderer(
            Rtx::RendererOptions{
                .mShaderDirectory = request.mShaderDirectory,
                .mWidth = request.mWidth,
                .mHeight = request.mHeight,
                .mUpscale = request.mUpscale,
                .mPreset = request.mPreset,
                .mWindow = window == nullptr ? nullptr : window->getHandle(),
                .mValidation = validation,
            },
            reason);
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

        out() << std::format(
            ", upscale {}, preset {}", Rtx::upscaleName(request.mUpscale), Rtx::presetName(request.mPreset));

        // **Said before the run rather than after it.** A figure measured under the layers is not
        // one to compare against anything, and finding that out at the end is finding it out after
        // the ten minutes have been spent.
        if (renderer->isValidating())
            out() << ", WITH THE VALIDATION LAYERS ON — pass --validation=false for a number worth quoting";

        out() << "\n";

        std::vector<BenchPlace> places;
        places.reserve(request.mViews.size());

        std::vector<double> frameTimes;
        std::vector<double> traceTimes;
        std::vector<double> placeTimes;
        frameTimes.reserve(measured);
        traceTimes.reserve(measured);
        placeTimes.reserve(measured);

        bool stopped = false;

        for (const View& view : request.mViews)
        {
            const ESM::Cell* cell = world.findCell(view.mCell);
            if (cell == nullptr)
            {
                out() << "\n" << view.mName << ": no cell called \"" << view.mCell << "\"\n";
                return 1;
            }

            if (window != nullptr)
                window->setTitle("OpenMW RTX - bench - " + view.mName);

            StagedWorld staged(world, *cell,
                StagingRequest{

                    .mWeather = request.mWeather,
                    .mHour = request.mHour,
                    .mDay = request.mDay,
                    .mFieldOfView = request.mFieldOfView,
                    .mOrigin = view.mOrigin,
                    .mTarget = view.mTarget,
                },
                request.mActors);

            if (staged.empty())
            {
                out() << "\n" << view.mName << ": the region placed no geometry\n";
                return 1;
            }

            Rtx::SceneUploader uploader;

            const Clock::time_point buildStart = Clock::now();
            uploader.hand(*renderer, Rtx::sWorld, staged.getScene(), world.getImageManager(), Rtx::SeaState{});
            const double buildMs = std::chrono::duration<double, std::milli>(Clock::now() - buildStart).count();

            const float far = std::max(staged.getScene().getBounds().radius() * 8.0f, 10000.0f);

            frameTimes.clear();
            traceTimes.clear();
            placeTimes.clear();

            std::uint32_t crossings = 0;
            std::uint32_t crossRebuilds = 0;
            double crossWorstMs = 0.0;
            double crossReadMs = 0.0;
            double crossBuildMs = 0.0;
            float part = 0.0f;

            // Per place, because the zones a place has are the zones its content asked for: an
            // interior with nothing moving in it never places and never reports one.
            Rtx::GpuBreakdown gpu;

            std::uint32_t hits = 0;

            // Restarted when the warmup ends, so `mWallSeconds` covers the frames `mFrames`
            // counts. A clock left running from here would divide six hundred frames by the time
            // six hundred and sixty took, and the sixty are the slow ones.
            Clock::time_point runStart = Clock::now();

            for (std::uint32_t frame = 0; frame < warmup + measured; ++frame)
            {
                // Pumped outside the timing: SDL is what keeps a window being drawn rather than
                // reported as hung, and it is no part of what the renderer costs.
                if (interrupted())
                {
                    stopped = true;
                    break;
                }

                if (frame == warmup)
                {
                    runStart = Clock::now();
                    profiling.enable();
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
                if (view.mRoute.has_value() && frame >= warmup)
                {
                    part = view.mRoute->partAt(staged.getPlacement(), static_cast<float>(frame - warmup) / sStepRate);
                    standing = view.mRoute->at(staged.getPlacement(), part);
                }

                // **Inside the frame's own timing, because a crossing is a dropped frame.** Timing
                // it outside would report a smooth run with a load cost printed beside it, which is
                // the opposite of what a p99 is for.
                if (const Crossing crossed = staged.moveTo(standing.mOrigin); crossed.happened())
                {
                    const Clock::time_point read = Clock::now();
                    const Rtx::SceneUpload handed = uploader.hand(
                        *renderer, Rtx::sWorld, staged.getScene(), world.getImageManager(), Rtx::SeaState{});
                    const Clock::time_point built = Clock::now();

                    ++crossings;
                    crossRebuilds += handed.mKind == Rtx::SceneUpload::Kind::Rebuilt ? 1u : 0u;
                    crossReadMs += std::chrono::duration<double, std::milli>(read - frameStart).count();
                    crossBuildMs += std::chrono::duration<double, std::milli>(built - read).count();
                    crossWorstMs
                        = std::max(crossWorstMs, std::chrono::duration<double, std::milli>(built - frameStart).count());
                }

                // **After the first, which is the frame the build above already made**, and by
                // frame index rather than by the clock: a world stepped by how long the last frame
                // took would render a different sequence on every machine and on every build.
                //
                // Handed rather than placed because a step walks the whole graph and sweeps it: an
                // actor drawing a weapon brings a mesh nothing has built, and a sweep that closed a
                // gap renumbers what the last frame was built from.
                double placeMs = 0.0;
                if (frame > 0 && staged.getMotion() != nullptr && staged.getMotion()->step(frame))
                {
                    const Clock::time_point placeStart = Clock::now();
                    uploader.hand(*renderer, Rtx::sWorld, staged.getScene(), world.getImageManager(), Rtx::SeaState{});
                    placeMs = std::chrono::duration<double, std::milli>(Clock::now() - placeStart).count();
                }

                Framing framing = Framing::lookingFrom(standing);
                framing.mFieldOfView = request.mFieldOfView;
                framing.mFar = far;
                framing.mDelight = request.mDelight;

                framing.mLighting = staged.getLighting();
                framing.mLighting.mSeconds = static_cast<float>(frame) / sStepRate;

                // What the upscaler's sample sequence and every random draw in the shader are walked
                // by. Held to the frame index so the same run draws the same samples twice over.
                framing.mFrame = frame;

                const Rtx::FrameResult result
                    = renderer->renderFrame(makeFrameConstants(framing, renderer->getExtents()),
                        Rtx::FrameOptions{ .mFilter = request.mFilter, .mExposure = request.mExposure });

                if (window != nullptr && !renderer->presentFrame())
                    renderer->resize(window->getWidth(), window->getHeight());

                const double frameMs = std::chrono::duration<double, std::milli>(Clock::now() - frameStart).count();

                if (frame >= warmup)
                {
                    frameTimes.push_back(frameMs);
                    traceTimes.push_back(result.mTraceMs);
                    placeTimes.push_back(placeMs);
                    gpu.add(result.mGpu);
                    hits = result.mHits;
                }
            }

            const Clock::time_point runEnd = Clock::now();
            profiling.disable();

            if (frameTimes.empty())
                break;

            const Rtx::FrameExtents traced = renderer->getExtents();
            const double pixels = static_cast<double>(traced.mRenderWidth) * traced.mRenderHeight;

            // Once: summarising sorts the rows in place, so a second call would be re-sorting what
            // the first one's iterators point at.
            const std::span<const Rtx::GpuZone> zones = gpu.summariseZones();

            places.push_back(BenchPlace{
                .mView = view.mName,
                .mCell = view.mCell,
                .mNote = view.mNote,
                .mBuildMs = buildMs,
                .mFrames = static_cast<std::uint32_t>(frameTimes.size()),
                .mWallSeconds = std::chrono::duration<double>(runEnd - runStart).count(),
                .mFrame = Rtx::summarise(frameTimes),
                .mTrace = Rtx::summarise(traceTimes),
                .mPlace = Rtx::summarise(placeTimes),
                .mGpu = std::vector<Rtx::GpuZone>(zones.begin(), zones.end()),
                .mHitPercent = static_cast<double>(hits) / pixels * 100.0,
                .mCrossings = crossings,
                .mCrossRebuilds = crossRebuilds,
                .mCrossWorstMs = crossWorstMs,
                .mCrossReadMs = crossReadMs,
                .mCrossBuildMs = crossBuildMs,
                .mTravelled = view.mRoute.has_value() ? static_cast<double>(part) : 1.0,
                .mScene = renderer->getSceneStats(),
            });

            report(places.back());
        }

        if (places.empty())
        {
            out() << "\nstopped before anything was measured\n";
            return 1;
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

        return 0;
    }
}
