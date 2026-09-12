#pragma once

#include <array>
#include <optional>
#include <string_view>
#include <utility>

#include "namedenum.hpp"
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
        /// nearly every upscaled frame, since `FrameOptions::mFilter` is on by default; worth saying
        /// only to a caller that knows the switch was given outright.
        bool mFilterSuppressed = false;

        /// The frame jittered although nothing asked it to, because an upscaler always jitters:
        /// reconstruction across several frames of one sample point is reconstruction from one
        /// sample.
        bool mJitterForced = false;

        /// Whether the wavelet ran over the indirect channel — one comparison, because the backend
        /// records the accumulator where this holds and `FrameImage::Accumulated` exists only where
        /// it did.
        bool filtered() const { return mDenoiser == Denoiser::Wavelet; }

        /// The whole of the rule, and the only copy of it.
        static Reconstruction resolve(const Upscaling& upscaling, const ReconstructionRequest& asked)
        {
            if (upscaling.mMode == Upscale::Off)
            {
                return Reconstruction{
                    .mDenoiser = asked.mFilter ? Denoiser::Wavelet : Denoiser::None,
                    .mJitter = asked.mJitter,
                };
            }

            return Reconstruction{
                .mDenoiser = Denoiser::RayReconstruction,
                .mUpscaling = upscaling,
                .mJitter = true,
                .mFilterSuppressed = asked.mFilter,
                .mJitterForced = !asked.mJitter,
            };
        }
    };

    /// Everything a run decides once about how the picture is made, in one bag for both hosts.
    /// Nothing here changes while a run is being made, so a frame reads what it was handed rather
    /// than asking the registry per knob per frame.
    struct RenderProfile
    {
        /// What the upscaler is built with. Carried whole into `RendererOptions`.
        Upscaling mUpscaling;

        /// What every frame asks of the reconstruction. Carried whole into `FrameOptions`.
        ReconstructionRequest mReconstruction;

        /// Whether the trace counts the see-through surfaces each primary ray crosses.
        bool mCountCrossings = false;

        /// How much of the painted lighting to divide out of a texture. Nought hands the trace
        /// Bethesda's textures with their lighting still in them.
        float mDelight = 1.0f;

        bool mShowAlbedo = false;

        /// What to scale the frame by before the display curve, or nothing to measure it off the
        /// frame. A picture wants it measured, and a reference wants it held still.
        std::optional<float> mExposure;
    };
}
