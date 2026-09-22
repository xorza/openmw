#ifndef OPENMW_COMPONENTS_RTX_SHADERS_ATROUS_H
#define OPENMW_COMPONENTS_RTX_SHADERS_ATROUS_H

#include "camera.h"
#include "hosttypes.h"
#include "look.h"
#include "portable.h"
#include "storageformat.h"

// What one wavelet level of the denoiser needs. Included verbatim by both sides, for the reason
// `visibility.h` is.

// What a level reads and writes, said once for both sides that have to agree.
//
// **The cascade's own, and not the trace's.** The levels ping-pong between the image the
// accumulator blended into and a scratch of this pass's own, so `CHANNEL_INDIRECT` is written once
// by the trace and read once by whatever consumes it. That is what lets the two formats part: a
// reference is built through that channel and never through this one.
//
// **Half floats, because a filtered bounce is shown and never summed.** A reference is built with
// the denoiser switched off, so nothing here reaches one — where the argument that holds
// `GBUFFER_RADIANCE` at full width is entirely about a term added to a thousand others.
//
// **What it costs is a floor, and the floor is measured.** Five levels each round what they store,
// which puts about 3e-4 of the value under the cascade's own error — visible only where the cascade
// had already driven that error below it, which is a flat sheet under a smooth sky.
// `theFilterAndItsHistoryConvergeOnAGrazingSurface` is that scene, and it carries the pair of
// figures.

#define ATROUS_CHANNEL STORAGE_RGBA16F

#ifdef RTX_HOST
namespace Rtx::Shaders
{
#endif

    /// Where `atrous.comp` binds what it reads and writes in set 0, and how many there are. The
    /// shader's layout and the pass's own layout and writes are numbered by these and by nothing
    /// else, so the two cannot drift apart.
    const uint ATROUS_BIND_SOURCE = 0;
    const uint ATROUS_BIND_FILTERED = 1;
    const uint ATROUS_BIND_GUIDE = 2;
    const uint ATROUS_BIND_DEPTH = 3;
    const uint ATROUS_BIND_MOMENTS = 4;
    const uint ATROUS_BINDINGS = 5;

    /// Threads along each edge of a level's workgroup.
    const uint ATROUS_WORKGROUP = 8;

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

        /// The spacing of this level's taps, in pixels. The three sigmas the taps are weighed by
        /// are `look.h`'s, because nothing varies them per level or per frame.
        uint mStep;
    };

    // Pinned for the reason `scene.h` gives: the side that writes these bytes and the side that
    // reads them are different compilers.
#ifdef RTX_HOST
    static_assert(sizeof(AtrousConstants) == 64, "AtrousConstants must be scalar-packed on every side");
#endif

#ifdef RTX_HOST
}
#endif

#endif
