// `#pragma once` everywhere else in this tree, and an include guard here: `glslc` warns
// "'#pragma once' : not implemented" and carries on, so a header included twice by one
// shader would redefine everything in it.
#ifndef OPENMW_COMPONENTS_RTX_SHADERS_COMPOSITE_H
#define OPENMW_COMPONENTS_RTX_SHADERS_COMPOSITE_H

#include "portable.h"

// What the last pass needs to turn the trace's separate channels back into one picture. Included
// verbatim by both sides, for the reason `visibility.h` is.

#ifdef RTX_HOST

#include <cstdint>

namespace Rtx::Shaders
{
    using uint = std::uint32_t;

#endif

    /// Threads along each edge of the composite's workgroup.
    RTX_CONST uint COMPOSITE_WORKGROUP = 8;

    /// Everything the recombination needs, which is almost nothing.
    ///
    /// **The trace left the hard part done.** Water's absorption and the fog's transmittance were
    /// folded into the modulation term where they belong, so what is left here is one multiply and
    /// one add — which is why the filter can sit between the two without knowing anything about
    /// either. The display curve is not here; it is the last pass, after whatever upscales.
    struct CompositeConstants
    {
        uint mWidth;
        uint mHeight;

        /// How many frames have gone into the running sum, this one included. Zero is no averaging.
        ///
        /// **Here rather than in the trace, and that placement is a decision.** What a reference has
        /// to converge to is the frame as it will be shown, filter and all — so the sum is taken
        /// after the filter, and turning the filter off is what produces the unfiltered reference
        /// the filter is then judged against. It is summed in linear, before the curve, for the
        /// reason the shader gives.
        uint mAccumulate;
    };

#ifdef RTX_HOST
}
#endif

#endif
