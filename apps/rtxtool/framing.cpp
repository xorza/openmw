#include "framing.hpp"

#include <components/rtx/camera.hpp>
#include <components/rtx/renderer.hpp>

namespace RtxTool
{
    Framing Framing::lookingFrom(const Placement& placement)
    {
        return Framing{ .mOrigin = placement.mOrigin, .mForward = placement.mTarget - placement.mOrigin };
    }

    Rtx::Shaders::VisibilityConstants makeFrameConstants(const Framing& framing, const Rtx::FrameExtents& extents)
    {
        // The render extent and not the output one: the trace runs at whatever the upscaler asked
        // for, and the camera's per-pixel ray spread is derived from it.
        Rtx::Shaders::VisibilityConstants constants = Rtx::makeCameraAlong(framing.mOrigin, framing.mForward,
            framing.mFieldOfView, extents.mRenderWidth, extents.mRenderHeight, framing.mFar);

        constants.mShowAlbedo = framing.mShowAlbedo ? 1u : 0u;
        constants.mDelight = framing.mDelight;
        constants.mFrame = framing.mFrame;
        applyLighting(framing.mLighting, constants);

        return constants;
    }
}
