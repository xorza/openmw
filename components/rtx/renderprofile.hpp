#pragma once

#include <optional>

#include "reconstruction.hpp"

namespace Rtx
{
    /// Everything a run decides once about how the picture is made.
    ///
    /// **One bag, and not a settings category, a command-line record and an option struct per
    /// host.** Split that way, every field is spelled three times and the harness hands its half
    /// across through the settings singleton, which is a message passed through a global.
    ///
    /// Nothing here changes while a run is being made, which is why a frame reads what it was handed
    /// rather than asking the registry per knob per frame.
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
