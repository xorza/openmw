// `#pragma once` everywhere else in this tree, and an include guard here: `glslc` warns
// "'#pragma once' : not implemented" and carries on, so a header included twice by one
// shader would redefine everything in it.
#ifndef OPENMW_COMPONENTS_RTX_SHADERS_EXPOSURE_H
#define OPENMW_COMPONENTS_RTX_SHADERS_EXPOSURE_H

#include "look.h"
#include "portable.h"

// What the two passes that measure a frame's brightness need. Included verbatim by both sides, for
// the reason `visibility.h` is.

#ifdef RTX_HOST

#include <cstdint>

namespace Rtx::Shaders
{
    using uint = std::uint32_t;

#endif

    /// Bins in the log-luminance histogram.
    ///
    /// **A histogram and not a running mean, because of what an interior looks like**: a handful of
    /// tiny flames at a luminance of one, in a room sitting at a hundredth of that. A mean is
    /// dragged around by whichever population has more pixels; a histogram keeps them apart and
    /// lets the reduction decide what to expose for.
    const uint EXPOSURE_BINS = 256;

    /// Threads along each edge of the binning pass's workgroup. Squared, it is `EXPOSURE_BINS`, so
    /// each thread owns exactly one bin of the workgroup's own tally.
    const uint HISTOGRAM_WORKGROUP = 16;

    /// What the binning pass needs to place a luminance.
    struct HistogramConstants
    {
        uint mWidth;
        uint mHeight;
    };

    /// What the reduction needs to undo the binning.
    struct ExposureConstants
    {
        /// Pixels binned, so the black bin can be discounted from the divisor.
        uint mPixels;

        /// Seconds since the previous measurement, which is what makes the approach a rate rather
        /// than a fraction per frame.
        float mElapsed;

        /// One where there is no previous exposure to move away from — the first frame, and any
        /// frame the renderer was told has no past. The measured value is taken outright.
        uint mReset;

        /// What to multiply the measured target by before anything approaches it. See
        /// `Rtx::Daylight::mExposureBias`.
        float mBias;
    };

#ifdef RTX_HOST
}
#endif

#endif
