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

    /// Where a value that is not finite does its harm: the boundaries a frame writes across into
    /// a history or hands to the denoiser, each a word of `FrameCounts::mNotFinite`. The fog
    /// volume's froxel, which is blended with its own history and read by its neighbours; the
    /// colour, which the denoiser accumulates and the wavelet path filters; and the guides beside
    /// it — albedo, specular, normal and roughness, motion, reflection motion, depth — which steer
    /// the denoiser's history.
    const uint BOUNDARY_FOG = 0u;
    const uint BOUNDARY_COLOUR = 1u;
    const uint BOUNDARY_GUIDE = 2u;
    const uint BOUNDARY_COUNT = 3u;

    struct FrameCounts
    {
        /// Primary rays that reached nothing, summed by the sky's miss shader where the trace was
        /// built to count — `COUNTING`. The host reports the hits, which are the launch less
        /// these: `FrameRing::finishOldest`.
        uint mMisses;

        /// What the hold's own clock said the hold came to, in nanoseconds, written by the loop
        /// `check` appends to the frame — `stress.comp`. Left alone by a frame with no hold.
        uint mHeldNs;

        /// Stores whose value was a NaN or an infinity, one word a boundary, summed by the pass
        /// that wrote them where the trace was built to count — `countNotFinite`. A history that
        /// took one writes it again every frame, so the count says the frame carries one whether
        /// or not this frame made it. `Check::Finite` asserts nought over a stop: a froxel that
        /// took `0 / 0` once spread across the whole frame in eight-pixel blocks, and nothing
        /// between the volume and the screen refused it.
        uint mNotFinite[BOUNDARY_COUNT];
    };

#ifdef RTX_HOST
    static_assert(sizeof(FrameCounts) == 20, "FrameCounts must be scalar-packed on every side");
}
#endif

#endif
