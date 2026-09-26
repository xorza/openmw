#pragma once

#include "reconstruction.hpp"
#include "shaders/visibility.h"

namespace Rtx
{
    struct FrameOptions;
    struct InstanceCounts;

    /// A frame as its trace samples it: what the caller stated, and every field that follows from
    /// the statement and the renderer's own state — the jitter, the noise and the level bias the
    /// reconstruction decides, how the surfaces are shown, what the scene holds, and where the eye
    /// was. The one place those are written, for a frame and for a picture inside the interface
    /// alike.
    ///
    /// **A statement that sets one of them is an assert.** They were written here over whatever the
    /// caller had put in them, which is a field with two writers and one of them losing silently:
    /// what a frame may ask of them, it asks through `options`.
    ///
    /// @param options what the frame asked for over `profile`; a picture's asks nothing.
    /// @param counts what the traced scene holds.
    /// @param previous the camera the last frame was traced with, to reproject against, or null for
    ///        a picture, which has no frame before it.
    Shaders::VisibilityConstants sampleFrame(const Shaders::VisibilityConstants& stated, const FrameOptions& options,
        const RenderProfile& profile, const Reconstruction& reconstruction, const InstanceCounts& counts,
        const Shaders::VisibilityConstants* previous);
}
