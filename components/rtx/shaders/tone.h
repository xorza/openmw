// `#pragma once` everywhere else in this tree, and an include guard here: `glslc` warns
// "'#pragma once' : not implemented" and carries on, so a header included twice by one
// shader would redefine everything in it.
#ifndef OPENMW_COMPONENTS_RTX_SHADERS_TONE_H
#define OPENMW_COMPONENTS_RTX_SHADERS_TONE_H

#include "portable.h"

// What the display pass needs. Included verbatim by both sides, for the reason `visibility.h` is.

#include "visibility.h"

#ifdef RTX_HOST

#include <cstdint>

namespace Rtx::Shaders
{

#endif

    /// Threads along each edge of the tone pass's workgroup.
    RTX_CONST uint TONE_WORKGROUP = 8;

    /// What the display pass is told: the size of the picture, and what is drawn on it that the
    /// trace could not draw.
    ///
    /// **Its own pass rather than the composite's last line, and the split is what upscaling
    /// needs.** An upscaler reconstructs from scene-referred radiance across several frames; a
    /// picture already squeezed through a display curve has had its highlights flattened into each
    /// other, and no amount of reconstruction gets them back. So the curve has to come after
    /// whatever upscales, at that pass's resolution and not the trace's — which is also why the
    /// sky's point sources are drawn here.
    struct ToneConstants
    {
        uint mWidth;
        uint mHeight;

        /// The trace's own extent, which is what the depth beside it is written at.
        ///
        /// **Two extents because an upscaler stands between them.** What this pass writes is one
        /// pixel of the picture; what it asks about a pixel — did this ray reach the sky — was
        /// answered at whatever the trace ran at, and at `performance` that is a quarter as many
        /// pixels.
        uint mTracedWidth;
        uint mTracedHeight;

        /// The frame's camera at *this* pass's extent, with no jitter.
        ///
        /// **The same basis and a different grid.** `rayAt` divides by the camera's own extent, so a
        /// camera carrying the output's is what turns an output pixel into the ray it shows. The
        /// jitter is the trace's: it moves a sample inside its pixel so an upscaler can accumulate
        /// several, and a pass that draws once at the resolution it is shown at wants the centre.
        Camera mCamera;

        /// The star field, drawn here rather than by the trace.
        ///
        /// **A point source is what a temporal upscaler removes.** Measured on a clear midnight at
        /// 1920 by 1080, the pixels over half brightness go 1093 drawn without one, 698 through DLAA
        /// at the same internal resolution, and 347 at quality — a third to the network and the rest
        /// to the resolution. No guide buffer moves it: an eye-facing normal, the bias mask over
        /// every sky pixel, and the four before-and-after colour pairs all measure neutral or worse.
        /// So the field is drawn where it is shown, and the trace draws the rest of the sky.
        StarField mStars;
    };

#ifdef RTX_HOST
}
#endif

#endif
