// `#pragma once` everywhere else in this tree, and an include guard here: `glslc` warns
// "'#pragma once' : not implemented" and carries on, so a header included twice by one
// shader would redefine everything in it.
#ifndef OPENMW_COMPONENTS_RTX_SHADERS_ATROUS_H
#define OPENMW_COMPONENTS_RTX_SHADERS_ATROUS_H

#include "camera.h"
#include "portable.h"

// What one wavelet level of the denoiser needs. Included verbatim by both sides, for the reason
// `visibility.h` is.

#ifdef RTX_HOST

#include <cstdint>

namespace Rtx::Shaders
{
    using uint = std::uint32_t;

#endif

    /// Threads along each edge of a level's workgroup.
    RTX_CONST uint ATROUS_WORKGROUP = 8;

    /// How far apart a level's taps stand, doubling each level: 1, 2, 4, 8, 16.
    ///
    /// **Five levels of a 5×5 kernel reach sixty-two pixels.** Each takes two taps at its own
    /// spacing, so the cascade's support is twice `1 + 2 + 4 + 8 + 16`. That is the à-trous trick —
    /// the holes between taps grow while the tap count does not, so a hundred and twenty-five
    /// samples do what a single kernel of that reach would need fifteen thousand for.
    RTX_CONST uint ATROUS_LEVELS = 5;

    /// Everything one level reads that is not an image.
    ///
    /// **The camera is here because the edge tests need world positions and the guide stores a
    /// distance.** A position is `origin + direction * distance`, and the difference between two of
    /// them drops the origin — so the basis is enough and the eye's place in the world is not
    /// needed. The rays are rebuilt by the same `rayAt` the trace built them with, which is what
    /// makes the reconstructed positions the ones that were actually shaded.
    struct AtrousConstants
    {
        Camera mCamera;

        /// The spacing of this level's taps, in pixels.
        uint mStep;

        /// How sharply the normals have to agree, as the exponent on their cosine.
        ///
        /// A hundred and twenty-eight keeps a tap at more than about six degrees of tilt from
        /// contributing anything, which is what stops a wall bleeding into the floor it meets.
        float mNormalPower;

        /// How far off the centre pixel's tangent plane a tap may sit, in pixel footprints.
        ///
        /// **Off the plane, not away from the eye.** Terrain seen at a grazing angle steps a long
        /// way in distance between neighbouring pixels while remaining one flat surface, so a test
        /// on distance alone would refuse to filter exactly the ground that most needs it.
        float mPlaneSigma;

        /// How far a tap's brightness may differ from the centre's before it stops being the same
        /// light, in standard deviations of what the centre has been measuring.
        ///
        /// **The term SVGF has and this cascade did not**, and the reason it did not was that there
        /// was no history to take a variance from. With one, the filter can finally stop at an edge
        /// in the *light* — the line where a shadow ends on a flat wall, which the normal test and
        /// the plane test both read as one surface and blur straight through.
        ///
        /// Scaled by the estimator's own spread, so a pixel that is still noisy filters widely and a
        /// settled one holds its detail. SVGF's own figure.
        float mLuminanceSigma;
    };

    // Pinned for the reason `scene.h` gives: the side that writes these bytes and the side that
    // reads them are different compilers.
#if defined(RTX_HOST) || defined(__METAL_VERSION__)
    static_assert(sizeof(AtrousConstants) == 76, "AtrousConstants must be scalar-packed on every side");
#endif

#ifdef RTX_HOST
}
#endif

#endif
