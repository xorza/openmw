#ifndef OPENMW_COMPONENTS_RTX_SHADERS_STRESS_H
#define OPENMW_COMPONENTS_RTX_SHADERS_STRESS_H

#include "hosttypes.h"
#include "portable.h"

// What the busy loop appended to a frame under `--stress-overlap` is told. Included verbatim by
// both sides, for the reason `visibility.h` is.

#ifdef RTX_HOST
namespace Rtx::Shaders
{
#endif

    /// Lanes in the one workgroup the loop runs on. One group and not a frame's worth, because
    /// what the loop is for is to hold the queue for a stated time, and one group does that with
    /// the rest of the card idle — the frame behind it is what the hold is supposed to overlap.
    const uint STRESS_WORKGROUP = 64;

    struct StressConstants
    {
        /// How many times round the loop each lane goes.
        uint mIterations;
    };

#ifdef RTX_HOST
}
#endif

#endif
