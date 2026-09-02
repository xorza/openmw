#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

#include <osg/Vec3f>

#include "framerequest.hpp"
#include "lighting.hpp"
#include "motion.hpp"

namespace Resource
{
    class ImageManager;
}

namespace Rtx
{
    class SceneDesc;
}

namespace RtxTool
{
    /// Everything the screenshot needs that is not the world itself.
    struct ShotRequest
    {
        FrameRequest mFrame;

        std::filesystem::path mOutput;

        /// What moves between traced frames, or null for a world that holds still. Borrowed for the
        /// length of the call.
        Motion* mMotion = nullptr;

        /// How many seconds the water has been moving. `StagingRequest::mSeaSeconds` says why.
        float mSeaSeconds = 0.0f;

        /// Whether each frame samples a different point inside its pixel.
        ///
        /// Only worth anything to something putting several frames together: with `--accumulate` it
        /// turns a converged reference into an antialiased one, and it is what an upscaler
        /// reconstructs detail from.
        bool mJitter = false;

        /// Write the albedo with no shading over it.
        bool mShowAlbedo = false;

        /// Report the share of pixels whose accumulated bounce luminance passes each of a ladder of
        /// thresholds, beside the frame's other figures.
        ///
        /// **What a firefly is counted in, and the one thing bytes cannot say.** A bright bounce is
        /// scene-referred radiance and the display curve has spent that by the time a pixel is a
        /// byte, so the tail is read off `Channel::Accumulated` — the bounce in linear radiance,
        /// after the accumulator has had it and before the albedo is put back on.
        ///
        /// Wants `--upscale=off` so the wavelet and its accumulator run at all, and a `--accumulate`
        /// long enough for the history to pass `ACCUMULATE_SETTLED`, which is what the clamp holds
        /// off until.
        bool mTail = false;

        /// Where to write the frame in linear radiance, or empty for none.
        ///
        /// **Four floats a pixel, raw, at the render extent.** A PNG is what a picture is looked at
        /// as; this is what one is *measured* against, and the two are not the same file — eight bits
        /// after the display curve is where the filter figures stopped being figures. `readPixels`
        /// gives the first and `Channel::Radiance` the second.
        std::filesystem::path mDump;

        /// How many times to trace the same frame before reporting on it.
        ///
        /// **One submit measures the clock, not the shader.** This machine's GPU idles at 315 MHz
        /// and ramps only under load, so the same frame from a cold start has timed anywhere between
        /// 0.37 and 2.1 ms — a spread wider than most changes worth making, and wide enough that two
        /// runs of *identical* code disagree by more than an A and a B do.
        ///
        /// **And it is high as well as unstable**, which is the half that misleads rather than
        /// merely frustrates: the view whose cold submit read 0.485 ms settles at 0.19 once the
        /// first submit's own costs are behind it. Tracing repeatedly inside one device session is
        /// what makes a difference visible — the best run is the least contended, and the spread
        /// beside it says whether to believe it.
        ///
        /// Eight costs about four milliseconds against a quarter of a second of device setup, so the
        /// default is the honest number rather than the fast one. A real comparison wants hundreds.
        /// A shot traces at least once whatever this says, because a shot is a picture.
        std::uint32_t mRepeat = 8;

        /// How many differently-seeded frames to average into the picture. Zero leaves `mRepeat` in
        /// charge, and the picture is one frame however many times that traced it.
        ///
        /// **A converged reference, which is the only ground truth a sampled renderer has.** One
        /// bounce per pixel estimates an integral without bias, so enough of them average to the
        /// value itself — and there is nothing else to compare a denoised frame against, since the
        /// answer cannot be written down. Error falls as the square root of this, so four times the
        /// frames halves it: a hundred is a clean picture and a thousand is a reference.
        ///
        /// **Not the same knob as `mRepeat`**, which traces one frame over and over to time it. This
        /// advances the seed, so every trace is a different sample and the picture improves; timing
        /// a run of these measures the accumulation as well as the trace.
        std::uint32_t mAccumulate = 0;

        /// Filled in from the cell once it has been read, which is why both commands take their
        /// request by value.
        CellLighting mLighting;

        /// Where to stand and what to look at. Both default to a view of the whole cell from outside
        /// it, which is the only placement that needs nothing known about the cell.
        std::optional<osg::Vec3f> mOrigin;
        std::optional<osg::Vec3f> mTarget;
    };

    /// Renders `scene` and writes a PNG. Returns a process exit status.
    ///
    /// Reports the fraction of primary rays that hit something, which is what tells "the cell
    /// rendered" from "the camera faced away from it" without anyone opening the file.
    int renderShot(Rtx::SceneDesc& scene, Resource::ImageManager& images, const Rtx::ValidationOptions& validation,
        const ShotRequest& request);
}
