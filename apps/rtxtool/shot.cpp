#include "shot.hpp"

#include "framing.hpp"
#include "placement.hpp"
#include <components/rtx/error.hpp>
#include <components/rtx/frametimes.hpp>
#include <components/rtx/png.hpp>
#include <components/rtx/shaders/colour.h>

#include <array>
#include <chrono>
#include <fstream>
#include <memory>
#include <ostream>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include <components/debug/debugging.hpp>
#include <components/files/conversion.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/sceneuploader.hpp>

namespace RtxTool
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        double millisecondsSince(Clock::time_point start)
        {
            return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        }

    }

    int renderShot(Rtx::SceneDesc& scene, Resource::ImageManager& images, const Rtx::ValidationOptions& validation,
        const ShotRequest& request)
    {
        std::ostream& out = Debug::getRawStdout();

        if (scene.getPlacedCount() == 0)
        {
            out << "Nothing to render: the cell placed no geometry.\n";
            return 1;
        }

        // Walks every mesh, so it is asked for once and the answer kept.
        const osg::BoundingBoxf bounds = scene.getBounds();
        const Placement placement = placeCamera(bounds, request.mFieldOfView, request.mOrigin, request.mTarget);

        const Clock::time_point deviceStart = Clock::now();
        std::string reason;
        const std::unique_ptr<Rtx::Renderer> renderer = Rtx::createRenderer(
            Rtx::RendererOptions{
                .mShaderDirectory = request.mShaderDirectory,
                .mWidth = request.mWidth,
                .mHeight = request.mHeight,
                .mUpscale = request.mUpscale,
                .mPreset = request.mPreset,
                .mValidation = validation,
            },
            reason);
        if (renderer == nullptr)
        {
            out << reason << '\n';
            return 1;
        }
        const double deviceMs = millisecondsSince(deviceStart);

        // **Asked rather than assumed.** `--size` is what the picture comes out at; what the trace
        // runs at is the upscaler's answer for that, and the camera's per-pixel ray spread is
        // derived from it.
        const Rtx::FrameExtents extents = renderer->getExtents();

        // **Staged and not streamed**: this renders the region once, so every composite has to be
        // finished here rather than drained over frames that will never come.
        Rtx::SceneUploader uploader;
        uploader.setStaged(true);

        const Clock::time_point buildStart = Clock::now();
        uploader.hand(*renderer, Rtx::sWorld, scene, images, Rtx::SeaState{});
        const double buildMs = millisecondsSince(buildStart);
        const Rtx::SceneStats& stats = renderer->getSceneStats();

        Framing framing = Framing::lookingFrom(placement);
        framing.mFieldOfView = request.mFieldOfView;

        // Far enough to cross any cell: the largest exterior view in the game is a few tens of
        // thousands of units, and a primary ray that reaches this has left the world. No floor under
        // it here, unlike `bench` and `view` — see `Framing::mFar`.
        framing.mFar = bounds.radius() * 8.0f;
        framing.mLighting = request.mLighting;
        framing.mDelight = request.mDelight;
        framing.mShowAlbedo = request.mShowAlbedo;

        // **Accumulating replaces repeating rather than joining it.** A run of `--accumulate=4` that
        // also honoured the repeat default would quietly average eight frames and report four — and
        // a convergence ladder built on that reads as though the first four samples bought nothing,
        // which is exactly what it looked like. A shot traces at least once either way, because a
        // shot is a picture.
        const bool averaging = request.mAccumulate > 0;
        const std::uint32_t frames = averaging ? request.mAccumulate : std::max(request.mRepeat, 1u);

        // **Whether each trace is a new sample or the same one again**, which is not the same
        // question as whether the frames are being averaged. An upscaler reconstructs from where
        // each frame sampled inside its pixel, so a run of traces at one seed hands it the same
        // sample over and over — the frames still resolve, and the extra detail they were supposed
        // to carry was never in them.
        const bool sequenced = averaging || request.mUpscale != Rtx::Upscale::Off;

        std::vector<double> traces;
        traces.reserve(frames);

        // **Across the repeats and not from the last frame.** The whole reason a shot traces more
        // than once is that one submit times the GPU's clock rather than the shader, and a
        // breakdown taken from a single frame would carry exactly that noise into every row.
        Rtx::GpuBreakdown gpu;

        std::uint32_t hits = 0;
        Rtx::Reconstruction reconstruction;

        // **A `do` and not a `for`, for the reason `summarise` takes its argument by reference.** The
        // bound below is `max(..., 1)` and a `for` over it still leaves the compiler unable to prove
        // the body ran, so indexing what it filled becomes a hard warning. Looping this way says it
        // structurally.
        std::uint32_t frame = 0;
        do
        {
            // **After the first, which is the frame the build above already made.** Stepping before
            // it would put the shot one frame into the animation and make `--repeat=1` a different
            // picture from no repeat at all. Handed rather than placed because a step walks the
            // whole graph and sweeps it, and either half of that can change what has to be built.
            if (frame > 0 && request.mMotion != nullptr && request.mMotion->step(frame))
                uploader.hand(*renderer, Rtx::sWorld, scene, images, Rtx::SeaState{});

            // A plain timing run holds it still, so that what the spread shows is the machine and
            // not two frames that happened to sample different geometry.
            framing.mFrame = sequenced ? frame : 0;

            const Rtx::FrameResult result = renderer->renderFrame(makeFrameConstants(framing, extents),
                Rtx::FrameOptions{ .mAccumulate = averaging ? frame + 1 : 0,
                    .mJitter = request.mJitter,
                    .mFilter = request.mFilter,
                    .mExposure = request.mExposure });
            traces.push_back(result.mTraceMs);
            gpu.add(result.mGpu);
            hits = result.mHits;
            reconstruction = result.mReconstruction;
            ++frame;
        } while (frame < frames);

        const Rtx::FrameTimes trace = Rtx::summarise(traces);

        std::vector<std::uint8_t> pixels;
        renderer->readPixels(pixels);
        Rtx::writePng(request.mOutput, extents.mOutputWidth, extents.mOutputHeight, pixels);

        // What one verdict covers out of everything the micromaps do, as a percentage — nought for
        // a scene with no micromap in it rather than a division by the nothing they cover.
        const Rtx::MicromapTally& tally = stats.mMicromapTally;
        const double covered = tally.mOpaque + tally.mTransparent + tally.mUnknown;
        const auto share = [covered](double part) { return covered > 0.0 ? part / covered * 100.0 : 0.0; };

        // **The frame a measurement is taken on**, which is not the frame a picture is looked at.
        // Raw floats and no container: what reads this is a script computing an error against
        // another one, and every image format that carries floats would have to be decoded first.
        if (!request.mDump.empty())
        {
            std::vector<float> radiance;
            renderer->readChannel(Rtx::Channel::Radiance, radiance);

            std::ofstream file(request.mDump, std::ios::binary);
            file.write(reinterpret_cast<const char*>(radiance.data()),
                static_cast<std::streamsize>(radiance.size() * sizeof(float)));

            // **Thrown and not reported**, the way `writePng` above answers the same failure. A
            // measurement whose output did not get written and whose command still succeeded is a
            // script comparing against whatever was at that path before.
            if (!file)
                throw Rtx::Error("could not write " + Files::pathToUnicodeString(request.mDump));
        }

        // **The bounce's tail, in radiance and not in bytes.** A firefly is a bounce far enough
        // above what the pixel has been seeing to be an outlier, and that is a statement about
        // scene-referred light: the display curve has spent the range it lives in long before a
        // pixel is a byte. Read off the channel the accumulator wrote, so what is counted is what
        // the clamp has already been over.
        std::string tail;
        if (request.mTail)
        {
            std::vector<float> bounce;
            renderer->readChannel(Rtx::Channel::Indirect, bounce);

            // The ladder 4.1's table was taken on. One is about where the signal ends — a surface
            // seeing a full hemisphere of sky — and everything past it is the tail proper.
            static constexpr std::array<float, 5> sThresholds{ 0.5f, 1.0f, 8.0f, 32.0f, 64.0f };
            std::array<std::uint64_t, 5> over{};

            const std::size_t counted = bounce.size() / 4;
            for (std::size_t at = 0; at < counted; ++at)
            {
                // **The renderer's own weights and not a copy of them.** `colour.h` says why they
                // are shared: a second set would be a second idea of which of two things is
                // brighter, and this is what decides which of a frame's pixels are outliers.
                const float lit = bounce[at * 4] * Rtx::Shaders::LUMINANCE_WEIGHTS.x()
                    + bounce[at * 4 + 1] * Rtx::Shaders::LUMINANCE_WEIGHTS.y()
                    + bounce[at * 4 + 2] * Rtx::Shaders::LUMINANCE_WEIGHTS.z();

                for (std::size_t step = 0; step < sThresholds.size(); ++step)
                    if (lit > sThresholds[step])
                        ++over[step];
            }

            std::ostringstream line;
            line << "bounce tail:";
            for (std::size_t step = 0; step < sThresholds.size(); ++step)
                line << (step == 0 ? " >" : ", >") << sThresholds[step] << ' '
                     << (counted > 0 ? static_cast<double>(over[step]) / static_cast<double>(counted) * 100.0 : 0.0)
                     << '%';
            line << '\n';
            tail = line.str();
        }

        // Primary rays, so out of the pixels that were traced rather than the pixels written.
        const double fraction
            = static_cast<double>(hits) / (static_cast<double>(extents.mRenderWidth) * extents.mRenderHeight) * 100.0;

        out << "wrote " << Files::pathToUnicodeString(request.mOutput) << ' ' << extents.mOutputWidth << 'x'
            << extents.mOutputHeight;

        // Said whenever the two differ, because a trace time is a statement about the size it was
        // traced at and the file gives no hint of it.
        if (extents.mRenderWidth != extents.mOutputWidth || extents.mRenderHeight != extents.mOutputHeight)
            out << ", traced at " << extents.mRenderWidth << 'x' << extents.mRenderHeight;

        // **What put the frame back together, said out loud.** Two of the switches that reach this
        // renderer do not decide on their own — an upscaler denoises for itself and jitters
        // regardless — so a run that named `--filter` or `--jitter` and got neither used to comply
        // in silence, and nothing anywhere said which of the two denoisers had made the picture a
        // comparison was about to be drawn from.
        out << '\n' << "reconstruct: " << Rtx::denoiserName(reconstruction.mDenoiser);
        if (reconstruction.mDenoiser == Rtx::Denoiser::RayReconstruction)
            out << ", preset " << Rtx::presetName(reconstruction.mPreset) << ", "
                << Rtx::upscaleName(reconstruction.mUpscale);
        if (reconstruction.mFilterSuppressed)
            out << "; --filter did not apply, the upscaler denoises for itself";
        if (reconstruction.mJitterForced)
            out << "; jittered regardless, an upscaler always does";

        out << '\n'
            << "camera:     " << placement.mOrigin.x() << ", " << placement.mOrigin.y() << ", " << placement.mOrigin.z()
            << "  looking at " << placement.mTarget.x() << ", " << placement.mTarget.y() << ", "
            << placement.mTarget.z() << '\n'
            << "primary rays that hit: " << fraction << "%\n"
            << "instances:  " << stats.mInstances << ", of which " << stats.mCutoutInstances << " are cutouts ("
            << stats.mMicromappedInstances << " micromapped)\n"
            << "micromaps:  " << share(tally.mOpaque) << "% opaque, " << share(tally.mTransparent) << "% transparent, "
            << share(tally.mUnknown) << "% still asking\n"
            << tail << "structures: " << stats.mStructureBytes / 1024 << " KiB\n"
            << "tables:     " << stats.mTableBytes / 1024 << " KiB\n"
            << "textures:   " << stats.mTextureCount << " in " << stats.mTextureBytes / 1024 << " KiB\n"
            << "device up:  " << deviceMs << " ms\n"
            << "build:      " << buildMs << " ms\n"
            << "trace:      " << trace.mBest << " ms";

        if (averaging)
            out << " (best of " << frames << " accumulated; median " << trace.mMedian << ", worst " << trace.mWorst
                << ")";
        else if (frames > 1)
            out << " (best of " << frames << "; median " << trace.mMedian << ", worst " << trace.mWorst << ")";
        else
            out << " (one submit, including the wait)";

        // **Said out loud, because it more than doubles the number above it.** The layers are on by
        // default outside a Release build, and a figure measured under them is not one to compare
        // against anything. Measured over Balmora at 1080p, best of twenty: 3.5 ms with no layers,
        // 3.9 ms with core and synchronization validation, 8.1 ms with GPU-assisted on top — a tenth
        // for the first two and well over double for the third.
        //
        // Anyone quoting a trace time wants `--validation=false`, which turns the two finer switches
        // off along with the layers wherever they were only on by default.
        if (renderer->isValidating())
            out << ", with the validation layers on";

        out << '\n';

        // **What the device says about the same submit**, which is the only way to tell a slow trace
        // from a slow everything-else: the figure above is one wait around eight pieces of work.
        if (const std::span<const Rtx::GpuZone> zones = gpu.summariseZones(); !zones.empty())
        {
            out << "gpu:       ";
            for (const Rtx::GpuZone& zone : zones)
                out << ' ' << zone.mName << ' ' << zone.mTimes.mMedian;

            out << "  (median ms each)\n";
        }

        return 0;
    }
}
