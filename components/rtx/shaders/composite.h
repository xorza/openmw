#ifndef OPENMW_COMPONENTS_RTX_SHADERS_COMPOSITE_H
#define OPENMW_COMPONENTS_RTX_SHADERS_COMPOSITE_H

#include "hosttypes.h"
#include "portable.h"
#include "storageformat.h"

// What the last pass needs to turn the trace's separate channels back into one picture. Included
// verbatim by both sides, for the reason `visibility.h` is.

#ifdef RTX_HOST
namespace Rtx::Shaders
{
#endif

    /// Where `composite.comp` binds what it reads and writes in set 0, and how many there are. The
    /// shader's layout and the pass's own layout and writes are numbered by these and by nothing
    /// else, so the two cannot drift apart.
    const uint COMPOSITE_BIND_DIRECT = 0;
    const uint COMPOSITE_BIND_INDIRECT = 1;
    const uint COMPOSITE_BIND_ALBEDO = 2;
    const uint COMPOSITE_BIND_SUM = 3;
    const uint COMPOSITE_BIND_COLOUR = 4;
    const uint COMPOSITE_BINDINGS = 5;

/// What the running sum a reference is built in is kept as: full floats, for the reason
/// `GBUFFER_RADIANCE_SUMMED` is.
#define COMPOSITE_SUM_FORMAT STORAGE_RGBA32F

    /// Threads along each edge of the composite's workgroup.
    const uint COMPOSITE_WORKGROUP = 8;

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
