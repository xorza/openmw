#ifndef OPENMW_COMPONENTS_RTX_SHADERS_LINE_H
#define OPENMW_COMPONENTS_RTX_SHADERS_LINE_H

#include "camera.h"
#include "hosttypes.h"
#include "portable.h"

// The debug lines and triangles drawn over the picture: the navmesh, the pathgrid, the actors'
// paths, the recast mesh and the collision shapes, which the game builds as `osg::Geometry` under
// `Mask_Debug` and a ray cannot meet.
//
// **Rasterized, and depth-tested against the trace.** A line has no area for a ray to land on, so
// the one graphics pipeline beside the interface's draws them, projected through the frame's own
// camera by the inverse of `rayAt`, and each fragment is kept where it stands nearer than the
// surface the eye's ray met at that pixel — the traced depth, read at the nearest texel. A tool
// and not the picture: drawn after the curve and before the interface, in the display's own values
// the drawers painted them in.

#ifdef RTX_HOST
namespace Rtx::Shaders
{
#endif

    /// What both stages are told: the camera on the picture's own grid, where the eye stands, the
    /// near plane a vertex is clipped at, and the traced extent the depth channel is read over.
    struct LineConstants
    {
        Camera mCamera;
        vec3 mOrigin;
        float mNear;
        uvec2 mTraced;
    };

#ifdef RTX_HOST
    static_assert(sizeof(LineConstants) == 84, "LineConstants must be scalar-packed on every side");
}
#endif

#endif
