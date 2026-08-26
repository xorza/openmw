#pragma once

#include <optional>
#include <string_view>

#include "upscale.hpp"

namespace Rtx
{
    /// What put a frame's indirect light back together.
    ///
    /// **Three states and not two flags**, because the two the renderer used to derive were not
    /// independent: an upscaler denoises for itself, so asking for the wavelet as well was a
    /// contradiction that resolved silently. A frame is reconstructed by one of these or by none of
    /// them, and which one is a thing a run can be asked.
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

    /// How `denoiser` is spelled in a report.
    inline std::string_view denoiserName(Denoiser denoiser)
    {
        switch (denoiser)
        {
            case Denoiser::None:
                return "none";
            case Denoiser::Wavelet:
                return "wavelet";
            case Denoiser::RayReconstruction:
                return "ray-reconstruction";
        }

        return "none";
    }

    /// Which network Ray Reconstruction runs.
    ///
    /// **Named for the letters NVIDIA uses**, because that is what a driver release note and a bug
    /// report will say. Ray Reconstruction keeps its own set, distinct from super-resolution's — the
    /// vendored `nvsdk_ngx_defs_dlssd.h` retires A through C and names D and E, where the
    /// super-resolution enum in `nvsdk_ngx_defs.h` retires D as well and names J through M. Reading
    /// one for the other selects a network that does not exist and is reverted to the default.
    enum class Preset
    {
        /// Whatever the installed feature library picks, which has changed between SDK versions and
        /// again between the convolutional and transformer models. **Two runs are not comparable
        /// under this**, which is the whole reason the rest of the enum is here.
        Default,

        /// NVIDIA's preset D — what the SDK calls the default transformer model.
        D,

        /// NVIDIA's preset E — the latest transformer model, and the only one that accepts a
        /// depth-of-field guide.
        E,
    };

    /// How `preset` is spelled on a command line, in a setting file and in a report.
    inline std::string_view presetName(Preset preset)
    {
        switch (preset)
        {
            case Preset::Default:
                return "default";
            case Preset::D:
                return "d";
            case Preset::E:
                return "e";
        }

        return "default";
    }

    /// The preset `name` spells, or nothing where it spells none of them.
    ///
    /// Nothing rather than a default, for the reason `upscaleNamed` gives: a typo that silently
    /// selects a different network is a measurement of something nobody asked for.
    inline std::optional<Preset> presetNamed(std::string_view name)
    {
        if (name == "default")
            return Preset::Default;
        if (name == "d")
            return Preset::D;
        if (name == "e")
            return Preset::E;

        return std::nullopt;
    }

    /// What a caller asked of the reconstruction, before the upscaler has its say.
    struct ReconstructionRequest
    {
        /// Whether the wavelet was wanted over the indirect channel.
        bool mFilter = true;

        /// Whether the primary ray was wanted moved inside its pixel.
        bool mJitter = false;

        /// Which network to pin, where one runs at all.
        Preset mPreset = Preset::D;
    };

    /// What actually reconstructs a frame, worked out once from what was asked of it.
    ///
    /// **The rule lived in two expressions in the middle of the frame path and answered nobody.**
    /// `filtering = mFilter && !upscaling` and `jitter = mJitter || upscaling` are correct and are
    /// invisible: two command-line switches meant nothing unless a third was set a particular way,
    /// and no run said which of the two denoisers had produced the picture it was being judged on.
    /// Both halves of that are the same defect — a decision taken where it cannot be reported.
    ///
    /// So it is taken here instead, once, by a function of its inputs and nothing else. The renderer
    /// drives the frame from what this says, and a report prints the same value, so the two cannot
    /// come apart.
    struct Reconstruction
    {
        Denoiser mDenoiser = Denoiser::None;

        /// Off wherever nothing upscales, which is also every frame the wavelet can run in.
        Upscale mUpscale = Upscale::Off;

        /// Which network ran. `Default` where none did, rather than the preset nobody used.
        Preset mPreset = Preset::Default;

        /// Whether the primary ray moved inside its pixel this frame.
        bool mJitter = false;

        /// The wavelet was wanted and did not run, because an upscaler denoises for itself.
        ///
        /// **Not an error, and not silence either.** `FrameOptions::mFilter` is on by default, so
        /// this is true of nearly every upscaled frame and means only "the switch did not decide".
        /// What turns it into something worth saying is a caller that knows the switch was given
        /// outright, which is the caller's to know and not this struct's.
        bool mFilterSuppressed = false;

        /// The frame jittered although nothing asked it to, because an upscaler always jitters:
        /// reconstruction across several frames of one sample point is reconstruction from one
        /// sample.
        bool mJitterForced = false;

        /// The whole of the rule, and the only copy of it.
        static Reconstruction resolve(Upscale upscale, const ReconstructionRequest& asked)
        {
            if (upscale == Upscale::Off)
            {
                return Reconstruction{
                    .mDenoiser = asked.mFilter ? Denoiser::Wavelet : Denoiser::None,
                    .mUpscale = Upscale::Off,
                    .mPreset = Preset::Default,
                    .mJitter = asked.mJitter,
                };
            }

            return Reconstruction{
                .mDenoiser = Denoiser::RayReconstruction,
                .mUpscale = upscale,
                .mPreset = asked.mPreset,
                .mJitter = true,
                .mFilterSuppressed = asked.mFilter,
                .mJitterForced = !asked.mJitter,
            };
        }
    };
}
