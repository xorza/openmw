// `#pragma once` everywhere else in this tree, and an include guard here for the reason
// `portable.h` gives.
#ifndef OPENMW_COMPONENTS_RTX_SHADERS_ACCUMULATE_H
#define OPENMW_COMPONENTS_RTX_SHADERS_ACCUMULATE_H

#include "camera.h"
#include "look.h"
#include "portable.h"

// What the wavelet's temporal half needs. Included verbatim by both sides, for the reason
// `visibility.h` is.

// What each of the three histories is made of, said once for both sides that have to agree.
//
// **The pass's own, and not the G-buffer's.** A channel the trace writes and a history the denoiser
// keeps share nothing but a number of bits, and these three were built from `GBUFFER_RADIANCE` and
// `GBUFFER_GUIDE` — so narrowing a channel for the trace's sake silently narrowed a history whose
// evidence lies somewhere else entirely. `.notes/rtx/shader-review.md` §4 is that evidence.
//
// **Half floats for the mean and the surface, because neither builds a reference.** What put the
// radiance channels back to full width is an argument about rounding a term before adding it to a
// thousand others. A normal is compared against a neighbour's, and a mean is a running value
// replaced every frame rather than a thousand terms added into one. Measured across five views,
// settled and unsettled, no pixel of the bounce reaches 32 against the 65504 a half holds.
//
// **What the mean pays for it is a floor on how slowly it may move.** The average is exponential
// with `alpha = 1 / ACCUMULATE_FRAMES`, so a frame moves the stored value by a sixteenth of the
// difference — and where that sixteenth falls under half a quantisation step it rounds back to where
// it was. A half's step is between 2^-12 and 2^-11 of the value, so the average stalls on
// differences under 0.4 to 0.8 per cent of it. Measured, the cascade's error against a converged
// reference did not rise, and `filter.cpp` carries the pair.
//
// **And the moments stay full floats whatever the other two do.** `E[l²] - E[l]²` is a difference of
// two numbers that are nearly equal once a pixel has settled, and a format that rounds each of them
// separately loses the whole of what is left.
//
// A macro rather than a constant, for the reason `gbuffer.h` gives: a layout qualifier is a token
// GLSL reads before it parses anything, and `VK_FORMAT_*` is an enumerator, and the preprocessor is
// the one thing both languages share.

#ifdef RTX_HOST

#define ACCUMULATE_COLOUR VK_FORMAT_R16G16B16A16_SFLOAT
#define ACCUMULATE_SURFACE VK_FORMAT_R16G16B16A16_SFLOAT
#define ACCUMULATE_MOMENTS VK_FORMAT_R32G32B32A32_SFLOAT

#else

#define ACCUMULATE_COLOUR rgba16f
#define ACCUMULATE_SURFACE rgba16f
#define ACCUMULATE_MOMENTS rgba32f

#endif

#ifdef RTX_HOST

#include <cstdint>

namespace Rtx::Shaders
{
    using uint = std::uint32_t;

#endif

    /// Threads along each edge of the accumulator's workgroup.
    const uint ACCUMULATE_WORKGROUP = 8;

    /// What a level of the wavelet is handed, and what the accumulator writes for it.
    struct AccumulateConstants
    {
        /// The camera the frame was traced with. **The jitter is why this is here**: the motion
        /// vector is written against the jittered pixel centre the ray was actually aimed at, so
        /// undoing it needs the same offset added back.
        Camera mCamera;

        /// Non-zero where there is no history to reuse — the first frame, a resize, a door walked
        /// through. Every pixel then starts its count again.
        uint mReset;

        /// What a world distance is multiplied by before `surfaceOut` holds it, which is
        /// `ACCUMULATE_DISTANCE_RANGE` over the frame's far plane.
        ///
        /// **Here rather than in `Camera`, because it is a storage scale and not a depth range.**
        /// `camera.h` keeps `mFar` out on the grounds that a filter has no use for what a depth was
        /// written against, and that still holds — what this pass needs is a number that keeps a
        /// stored distance inside a half's proportional range, and it is only derived from the same
        /// value.
        float mDistanceScale;
    };

    // Pinned for the reason `scene.h` gives: the side that writes these bytes and the side that
    // reads them are different compilers.
#ifdef RTX_HOST
    static_assert(sizeof(AccumulateConstants) == 68, "AccumulateConstants must be scalar-packed on every side");
#endif

#ifdef RTX_HOST
}
#endif

#endif
