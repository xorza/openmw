#ifndef OPENMW_COMPONENTS_RTX_SHADERS_COUNTS_H
#define OPENMW_COMPONENTS_RTX_SHADERS_COUNTS_H

#include "hosttypes.h"
#include "portable.h"

// What a frame counts on the device for the host to read once the frame is waited for. One block
// for the frame, so the frame that clears it, the passes that write it and the ring that reads it
// back agree about where each word sits. Included verbatim by both sides, for the reason
// `visibility.h` is.

#ifdef RTX_HOST
namespace Rtx::Shaders
{
#endif

    struct FrameCounts
    {
        /// Primary rays that hit something, summed by the trace where it was built to count them
        /// — `COUNT_HITS`.
        uint mHits;

        /// What the hold's own clock said the hold came to, in nanoseconds, written by the loop
        /// `check` appends to the frame — `stress.comp`. Left alone by a frame with no hold.
        uint mHeldNs;
    };

#ifdef RTX_HOST
    static_assert(sizeof(FrameCounts) == 8, "FrameCounts must be scalar-packed on every side");
}
#endif

#endif
