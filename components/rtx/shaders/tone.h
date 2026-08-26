// `#pragma once` everywhere else in this tree, and an include guard here: `glslc` warns
// "'#pragma once' : not implemented" and carries on, so a header included twice by one
// shader would redefine everything in it.
#ifndef OPENMW_COMPONENTS_RTX_SHADERS_TONE_H
#define OPENMW_COMPONENTS_RTX_SHADERS_TONE_H

#include "portable.h"

// What the display pass needs. Included verbatim by both sides, for the reason `visibility.h` is.

#ifdef RTX_HOST

#include <cstdint>

namespace Rtx::Shaders
{
    using uint = std::uint32_t;

#endif

    /// Threads along each edge of the tone pass's workgroup.
    RTX_CONST uint TONE_WORKGROUP = 8;

    /// The size of the image being encoded, which is the only thing the curve needs to know.
    ///
    /// **Its own pass rather than the composite's last line, and the split is what upscaling
    /// needs.** An upscaler reconstructs from scene-referred radiance across several frames; a
    /// picture already squeezed through a display curve has had its highlights flattened into each
    /// other, and no amount of reconstruction gets them back. So the curve has to come after
    /// whatever upscales, at that pass's resolution and not the trace's.
    struct ToneConstants
    {
        uint mWidth;
        uint mHeight;
    };

#ifdef RTX_HOST
}
#endif

#endif
