#pragma once

#include <optional>

#include "reconstruction.hpp"
#include "reorder.hpp"
#include "upscale.hpp"

namespace Rtx
{
    /// Everything a run decides once about how the picture is made.
    ///
    /// **One bag, where this was five.** The same switches were a settings category, a command-line
    /// record in the harness, two option structs in the core and five loose members on the game's
    /// renderer — and the harness carried its half across by writing the settings singleton and
    /// letting the renderer read it back, which is a message passed through a global and every
    /// field spelled three times. `mExposure` lost its `std::optional` shape on the way through and
    /// was rebuilt from a sentinel on the far side.
    ///
    /// Nothing here changes while a run is being made, which is why a frame reads what it was handed
    /// rather than asking the registry per knob per frame.
    struct RenderProfile
    {
        Upscale mUpscale = Upscale::Off;
        Preset mPreset = Preset::D;
        Reorder mReorder = Reorder::Off;

        /// Whether the trace counts the see-through surfaces each primary ray crosses.
        bool mCountCrossings = false;

        /// How much of the painted lighting to divide out of a texture. Nought hands the trace
        /// Bethesda's textures with their lighting still in them.
        float mDelight = 1.0f;

        bool mShowAlbedo = false;
        bool mFilter = true;
        bool mJitter = false;

        /// What to scale the frame by before the display curve, or nothing to measure it off the
        /// frame. A picture wants it measured, and a reference wants it held still.
        std::optional<float> mExposure;
    };
}
