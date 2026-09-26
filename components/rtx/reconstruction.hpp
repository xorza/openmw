#pragma once

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

#include "frameextents.hpp"
#include "namedenum.hpp"
#include "surfaceview.hpp"
#include "upscale.hpp"

namespace Rtx
{
    /// What put a frame's indirect light back together — three states and not two flags, because
    /// an upscaler denoises for itself and asking for the wavelet as well is a contradiction that
    /// resolves silently.
    enum class Denoiser
    {
        /// The raw bounce, as the trace wrote it. What a converged reference is built from, because
        /// a thousand filtered frames converge on the filter's opinion rather than on the truth.
        None,

        /// The à-trous wavelet over the indirect channel.
        Wavelet,

        /// DLSS Ray Reconstruction, which denoises and upscales in one.
        RayReconstruction,
    };

    /// How a `Denoiser` is spelled in a report. The one list of the names, for the reason
    /// `sUpscaleNames` gives.
    inline constexpr NamedEnum sDenoiserNames{ std::array{
        std::pair{ Denoiser::None, std::string_view("none") },
        std::pair{ Denoiser::Wavelet, std::string_view("wavelet") },
        std::pair{ Denoiser::RayReconstruction, std::string_view("ray-reconstruction") },
    } };

    /// Which network Ray Reconstruction runs, named for the letters NVIDIA uses. Ray Reconstruction
    /// keeps its own set, distinct from super-resolution's: `nvsdk_ngx_defs_dlssd.h` names D and E,
    /// where `nvsdk_ngx_defs.h` names J through M, and reading one for the other selects a network
    /// that does not exist.
    enum class Preset
    {
        /// Whatever the installed feature library picks, which has changed between SDK versions and
        /// again between the convolutional and transformer models. Two runs are not comparable
        /// under this, which is the whole reason the rest of the enum is here.
        Default,

        /// NVIDIA's preset D — what the SDK calls the default transformer model.
        D,

        /// NVIDIA's preset E — the latest transformer model, and the only one that accepts a
        /// depth-of-field guide.
        E,
    };

    /// How a `Preset` is spelled on a command line, in a setting file and in a report. The one list
    /// of the names, for the reason `sUpscaleNames` gives.
    inline constexpr NamedEnum sPresetNames{ std::array{
        std::pair{ Preset::Default, std::string_view("default") },
        std::pair{ Preset::D, std::string_view("d") },
        std::pair{ Preset::E, std::string_view("e") },
    } };

    /// Where the trace's per-pixel draws come from — the shadow ray's place on the source, the
    /// bounce's direction, the fog's and the water's — as against the reservoirs, which step a
    /// hashed counter whichever this says.
    enum class NoiseSource
    {
        /// The blue-noise tile turned by the golden ratio each frame: an arrangement across the
        /// screen, which is what makes one sample per pixel filter well. `Rtx::BlueNoise` says why
        /// the wavelet wants it.
        BlueNoiseTile,

        /// A hashed counter seeded by the pixel, the frame and the stream: independent draws with
        /// no arrangement at all, which is what a network trained on independent samples asks
        /// for — the DLSS-RR integration guide, section 3.5, and the reason the tile is not
        /// handed to it.
        WhiteHash,
    };

    /// How a `NoiseSource` is spelled on a command line and in a report.
    inline constexpr NamedEnum sNoiseSourceNames{ std::array{
        std::pair{ NoiseSource::BlueNoiseTile, std::string_view("blue-noise") },
        std::pair{ NoiseSource::WhiteHash, std::string_view("white-hash") },
    } };

    /// Whether the launch sorts its threads by what they hit before the shader the hit names
    /// runs, and by what. A run's decision and not a frame's — `lib/variants.glsl` `REORDER` is
    /// the constant it becomes — and off until the bench says a hint pays.
    enum class Reorder
    {
        None,

        /// By the shader the hit names alone, which is the key the driver sorts on with no hint.
        Shader,

        /// By the shader and the low bits of the hit material's diffuse texture, for the sheet the
        /// shader is about to read.
        Texture,
    };

    /// How a `Reorder` is spelled on a command line and in a report.
    inline constexpr NamedEnum sReorderNames{ std::array{
        std::pair{ Reorder::None, std::string_view("none") },
        std::pair{ Reorder::Shader, std::string_view("shader") },
        std::pair{ Reorder::Texture, std::string_view("texture") },
    } };

    /// What the upscaler is built with, decided once per set of targets: the mode says whether an
    /// upscaler runs and at what ratio, and the preset which network it runs. A feature is created
    /// per resolution with both.
    struct Upscaling
    {
        Upscale mMode = Upscale::Off;

        /// Which network to pin, where one runs at all. Pinned rather than left to the library,
        /// whose default has changed between SDK versions, so that two runs are comparable.
        Preset mPreset = Preset::D;

        bool operator==(const Upscaling& other) const = default;
    };

    /// What a frame asks of the reconstruction, before the upscaler has its say.
    struct ReconstructionRequest
    {
        /// Whether the wavelet was wanted over the indirect channel.
        bool mFilter = true;

        /// Whether the primary ray was wanted moved inside its pixel.
        bool mJitter = false;

        /// Where the trace's draws come from, where a run names a source. Nothing hands the
        /// choice to `resolve`, which follows the denoiser; naming one is the A/B.
        std::optional<NoiseSource> mNoise;

        /// What is added to the texture level bias past the ratio the upscaler sets, in levels:
        /// the DLSS programming guide's epsilon (section 3.5), which a run walks on a sign and a
        /// book. Nought is the ratio alone. Without an upscaler the ratio is nought and this is
        /// the whole of the bias, which is what lets a test and an A/B read a level off the
        /// unupscaled path.
        float mLevelEpsilon = 0.0f;

        bool operator==(const ReconstructionRequest& other) const = default;
    };

    /// What actually reconstructs a frame, worked out once from what was asked of it, by a
    /// function of its inputs and nothing else: the renderer drives the frame from what this says
    /// and a report prints the same value, where `mFilter && !upscaling` in the middle of the
    /// frame path answered nobody.
    struct Reconstruction
    {
        Denoiser mDenoiser = Denoiser::None;

        /// What upscaled the frame: off wherever nothing did, which is also every frame the wavelet
        /// can run in, and then `Preset::Default` rather than the preset nobody used.
        Upscaling mUpscaling{ .mMode = Upscale::Off, .mPreset = Preset::Default };

        /// Whether the primary ray moved inside its pixel this frame.
        bool mJitter = false;

        /// The wavelet was wanted and did not run, because an upscaler denoises for itself. True of
        /// nearly every upscaled frame, since `ReconstructionRequest::mFilter` is on by default;
        /// worth saying only to a caller that knows the switch was given outright.
        bool mFilterSuppressed = false;

        /// The frame jittered although nothing asked it to, because an upscaler always jitters:
        /// reconstruction across several frames of one sample point is reconstruction from one
        /// sample.
        bool mJitterForced = false;

        /// Where the trace drew from: the tile under the wavelet or nothing, and the hash under an
        /// upscaler, unless the request named one. A consequence of the denoiser, because the
        /// tile is an arrangement the wavelet reads and the network does not want.
        NoiseSource mNoise = NoiseSource::BlueNoiseTile;

        /// What every texture level is offset by, in levels: the shown pixel's cone is narrower
        /// than the traced one by the upscaler's ratio, and a level chosen for the traced pixel
        /// reads every texture that much coarser than the picture shows. The DLSS programming
        /// guide, section 3.5: `log2(render / display)`, plus the request's epsilon. The ratio is
        /// nought where nothing upscales, and the epsilon stands on its own there.
        float mLevelBias = 0.0f;

        /// Whether the wavelet ran over the indirect channel — one comparison, because the backend
        /// records the accumulator where this holds.
        bool filtered() const { return mDenoiser == Denoiser::Wavelet; }

        /// Whether an upscaler reconstructed the frame.
        bool upscaled() const { return mUpscaling.mMode != Upscale::Off; }

        /// The whole of the rule, and the only copy of it.
        ///
        /// @param extents what the frame is traced at and shown at, read only under an upscaler
        ///        and only for its widths — the DLSS guide states its ratio in X, and the pixels
        ///        are square.
        static Reconstruction resolve(
            const Upscaling& upscaling, const ReconstructionRequest& asked, const FrameExtents& extents)
        {
            if (upscaling.mMode == Upscale::Off)
            {
                return Reconstruction{
                    .mDenoiser = asked.mFilter ? Denoiser::Wavelet : Denoiser::None,
                    .mJitter = asked.mJitter,
                    .mNoise = asked.mNoise.value_or(NoiseSource::BlueNoiseTile),
                    .mLevelBias = asked.mLevelEpsilon,
                };
            }

            return Reconstruction{
                .mDenoiser = Denoiser::RayReconstruction,
                .mUpscaling = upscaling,
                .mJitter = true,
                .mFilterSuppressed = asked.mFilter,
                .mJitterForced = !asked.mJitter,
                .mNoise = asked.mNoise.value_or(NoiseSource::WhiteHash),
                .mLevelBias = levelBiasOf(extents, asked.mLevelEpsilon),
            };
        }

    private:
        /// The guide's formula with the epsilon the request adds.
        static float levelBiasOf(const FrameExtents& extents, const float epsilon)
        {
            assert(extents.mRenderWidth > 0 && extents.mOutputWidth > 0 && "an upscaler with no extents to bias by");

            return std::log2(static_cast<float>(extents.mRenderWidth) / static_cast<float>(extents.mOutputWidth))
                + epsilon;
        }
    };

    /// How wide the radiance the trace writes is stored: `direct`, `indirect` and the composite's
    /// own frame.
    ///
    /// **Full floats where a reference sums them, and half floats where a frame is shown.** A
    /// reference is a sum of a thousand frames, and rounding every term before adding it only
    /// averages away if the error is random, which it is not — the direct light is all but
    /// identical from frame to frame and the sampler is a low-discrepancy sequence, so in halves
    /// the converged mean of a flat surface comes out low by more than a test's tolerance. A frame
    /// that is shown is never summed: the peak linear radiance a frame of this game reaches is
    /// under nine, which a half carries with four orders of magnitude to spare at a step finer than
    /// the display's — and sixteen bytes a pixel written three times and read six a frame is what
    /// the full width costs a picture that cannot tell.
    enum class RadianceWidth
    {
        Shown,
        Summed,
    };

    /// Everything a run decides once about how the picture is made, in one bag for both hosts,
    /// handed to the backend inside `RendererOptions` and read there. A frame reads what the run
    /// was handed rather than asking the registry per knob per frame, and only a menu moves it
    /// afterwards: `Renderer::setUpscale` changes `mUpscaling`, which `getProfile` then answers.
    /// What a frame is scaled by before the display curve: a fixed scale, or nothing to measure it
    /// off the frame.
    using ExposureRule = std::optional<float>;

    struct RenderProfile
    {
        /// What the upscaler is built with.
        Upscaling mUpscaling;

        /// What every frame asks of the reconstruction, before the upscaler has its say —
        /// `Reconstruction::resolve` is the rule. Jitter is off unless something puts the frames
        /// back together; the filter is off for a reference, because a thousand filtered frames
        /// converge on the filter's opinion. A frame may ask otherwise (`FrameOptions`).
        ReconstructionRequest mReconstruction;

        /// How much of the painted lighting to divide out of a texture. Nought hands the trace
        /// Bethesda's textures with their lighting still in them.
        float mDelight = 1.0f;

        /// What every pixel is painted with: the light, or a surface input for a picture of the
        /// maps themselves.
        SurfaceView mShow = SurfaceView::Shaded;

        /// What to scale the frame by before the display curve. A picture wants it measured, and a
        /// reference wants it held still. A frame may ask otherwise, like `mReconstruction`.
        ExposureRule mExposure;

        /// How long to hold the queue after every frame's trace, in milliseconds, or nought to
        /// hold it not at all. A held queue keeps the device that far behind the host, so every
        /// frame is recorded over a frame still running: what makes a hazard that needs the
        /// overlap show on the first frame of every run. `check` sets it, and so does the second
        /// leg of `repeat`. What the hold came to on each frame is the zone `sHoldZone` of the
        /// frame's report, which is what the run's `QueueHeld` check reads it back by.
        double mStressOverlapMs = 0.0;

        static constexpr std::string_view sHoldZone = "stress";

        /// Whether the trace keeps a launch per tuple of the frame's facts — the sun, the moons,
        /// the sea, `VisibilityVariant` — or one launch that carries every case. The picture is
        /// the same either way, by the argument `lib/variants.glsl` makes; what differs is the
        /// trace's time in a room and how many launches the driver compiles, sixteen against two.
        /// Off is an experiment — the harness's `--variants=false` — and not a setting.
        bool mSpecializeLaunches = true;

        /// How wide the radiance channels are stored, which `RadianceWidth` says is a question of
        /// whether a run sums its frames or shows them. The reference's width unless a run says
        /// it only shows its frames, so that a run that forgot to say is exact rather than fast.
        RadianceWidth mRadianceWidth = RadianceWidth::Summed;

        Reorder mReorder = Reorder::None;
    };
}
