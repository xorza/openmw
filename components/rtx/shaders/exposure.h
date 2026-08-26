// `#pragma once` everywhere else in this tree, and an include guard here: `glslc` warns
// "'#pragma once' : not implemented" and carries on, so a header included twice by one
// shader would redefine everything in it.
#ifndef OPENMW_COMPONENTS_RTX_SHADERS_EXPOSURE_H
#define OPENMW_COMPONENTS_RTX_SHADERS_EXPOSURE_H

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
    RTX_CONST uint EXPOSURE_BINS = 256;

    /// Threads along each edge of the binning pass's workgroup. Squared, it is `EXPOSURE_BINS`, so
    /// each thread owns exactly one bin of the workgroup's own tally.
    RTX_CONST uint HISTOGRAM_WORKGROUP = 16;

    /// Darkest luminance the histogram resolves, as a power of two. About a thousandth of mid grey,
    /// which is below anything a lit surface reaches and well under an unlit interior.
    RTX_CONST float MIN_LOG_LUMINANCE = -10.0;

    /// Brightest, as a power of two. Sixty-four times mid grey covers a flame seen directly.
    RTX_CONST float MAX_LOG_LUMINANCE = 6.0;

    /// Where a pixel stops being binned and starts being counted as black.
    ///
    /// Without it the dark areas of an interior pile into the lowest bin and drag the average down
    /// to meet them, and the exposure opens until the few lit surfaces are white.
    RTX_CONST float EXPOSURE_BLACK = 0.0001;

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
    };

#ifdef RTX_HOST
}
#endif

#endif
