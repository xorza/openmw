// `#pragma once` everywhere else in this tree, and an include guard here for the reason
// `portable.h` gives.
#ifndef OPENMW_COMPONENTS_RTX_SHADERS_ACCUMULATE_H
#define OPENMW_COMPONENTS_RTX_SHADERS_ACCUMULATE_H

#include "camera.h"
#include "portable.h"

// What the wavelet's temporal half needs. Included verbatim by both sides, for the reason
// `visibility.h` is.

#ifdef RTX_HOST

#include <cstdint>

namespace Rtx::Shaders
{
    using uint = std::uint32_t;

#endif

    /// Threads along each edge of the accumulator's workgroup.
    RTX_CONST uint ACCUMULATE_WORKGROUP = 8;

    /// The longest history a pixel may keep, in frames.
    ///
    /// **This is the one dial on the trade the accumulator exists to make**, and it is a trade
    /// rather than a setting with a right answer: a longer history is a quieter picture and a later
    /// one. The estimator's error falls as `1/sqrt(n)`, so the return on each further frame is
    /// shrinking while the lag it costs is not — and lag on a bounce shows up as light sliding off
    /// a wall a moment after the lamp that lit it moved.
    ///
    /// **Sixteen is chosen for the lag and not yet measured for the noise**, and saying so is the
    /// point: it is a quarter of a second at sixty frames, which is inside what a player reads as
    /// "the light is on the wall" rather than as a fade. What it is worth against the noise wants a
    /// sweep nobody has run: measured on the grid the filter tests use, sixteen frames take 44% of
    /// the error the spatial cascade cannot reach, but no other count has been tried against it.
    /// Until one is, this is a number picked from the half of the trade that can be reasoned about.
    RTX_CONST float ACCUMULATE_FRAMES = 16.0f;

    /// How far above the running mean a sample may sit before it is taken as an outlier rather than
    /// as light, in standard deviations.
    ///
    /// **A count of sigmas and not a radiance, which is the whole reason this waited for a history.**
    /// An absolute ceiling on the bounce cannot be derived — a lamp's intensity is content, and
    /// `falloff` hands a bounce that lands on one whatever that lamp was given. Against a mean and a
    /// variance the same question has a scene-independent answer: a sample this far from what the
    /// pixel has been seeing is not what the pixel is looking at.
    ///
    /// **Measured, with `shot --tail`**: sixteen accumulated frames take Seyda Neen's tail from 176
    /// pixels over 0.5 to ten, and the clamp takes those ten to three. Where it declines to fire is
    /// an interior full of lamps, because a pixel that sees a bright thing *consistently* raises the
    /// mean to meet it and is never an outlier — which is the design working, not failing.
    ///
    /// Four sigma leaves a Gaussian tail of one sample in sixteen thousand, which at sixteen frames
    /// of history is a clamp that fires on nothing that is really there.
    RTX_CONST float ACCUMULATE_SIGMAS = 4.0f;

    /// How many frames a pixel needs before its second moment describes a spread rather than a
    /// coincidence.
    ///
    /// **Under this the outlier clamp holds off and the cascade is told the pixel is as uncertain as
    /// a pixel can be.** Both are the same admission: a mean of two samples has a variance, and it
    /// is not one anybody should filter by.
    RTX_CONST float ACCUMULATE_SETTLED = 4.0f;

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
    };

    // Pinned for the reason `scene.h` gives: the side that writes these bytes and the side that
    // reads them are different compilers.
#if defined(RTX_HOST) || defined(__METAL_VERSION__)
    static_assert(sizeof(AccumulateConstants) == 64, "AccumulateConstants must be scalar-packed on every side");
#endif

#ifdef RTX_HOST
}
#endif

#endif
