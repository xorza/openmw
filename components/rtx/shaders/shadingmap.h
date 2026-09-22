#ifndef OPENMW_COMPONENTS_RTX_SHADERS_SHADINGMAP_H
#define OPENMW_COMPONENTS_RTX_SHADERS_SHADINGMAP_H

#include "hosttypes.h"
#include "look.h"
#include "portable.h"

// The shading estimate's dispatch: what `shadingmap.comp` is told about the texture it reads,
// and the one number the estimate is made with that both the dispatch and `Rtx::ShadingMap`
// have to agree on. `Rtx::ShadingMap` says what the estimate is and why.

#ifdef RTX_HOST
namespace Rtx::Shaders
{
#endif

    /// Where `shadingsum.comp` binds what it reads and writes in set 0, and how many there are. The
    /// shader's layout and the pass's own layout and writes are numbered by these and by nothing
    /// else, so the two cannot drift apart.
    const uint SHADING_SUM_BIND_SOURCE = 0;
    const uint SHADING_SUM_BIND_SUMS = 1;
    const uint SHADING_SUM_BINDINGS = 2;

    /// Where `shadingmap.comp` binds what it reads and writes in set 0, and how many there are. The
    /// shader's layout and the pass's own layout and writes are numbered by these and by nothing
    /// else, so the two cannot drift apart.
    const uint SHADING_MAP_BIND_SUMS = 0;
    const uint SHADING_MAP_BIND_MAP = 1;
    const uint SHADING_MAP_BINDINGS = 2;

    /// How many times the grid is box blurred, three by three and wrapping, before it is
    /// normalised: three passes are a close enough Gaussian for anything this coarse, and a
    /// correction with an edge in it would put that edge into the frame. Wrapping because
    /// Morrowind's textures tile, and a blur that clamped at the edges would invent a gradient
    /// across every wall.
    const uint SHADING_BLUR_PASSES = 3u;

    /// One factor as the map's format stores it, before the unorm's rounding: its place between
    /// the floor and the ceiling, so the neutral factor is exactly a third and a step is a part in
    /// forty thousand. `look.h` says why the range is the map's and not the format's. The one
    /// statement of the encode, which the dispatch stores and `Rtx::encodeShading` rounds.
    RTX_SHADER float shadingUnit(float factor)
    {
        const float unit = (factor - SHADING_FLOOR) / (SHADING_CEILING - SHADING_FLOOR);
        return unit < 0.0f ? 0.0f : (unit > 1.0f ? 1.0f : unit);
    }

    /// One cell's sum, as the summing stage leaves it for the map stage: the linear luminance of
    /// every texel that counted, and how many did.
    struct ShadingSum
    {
        float mSum;
        uint mCount;
    };

    /// What the dispatch is told about the texture's finest level.
    struct ShadingConstants
    {
        uint mWidth;
        uint mHeight;

        /// One where the texture is BC1, whose only alpha is a hole: a texel with none is one
        /// the estimate leaves out, as the host's `blockSum` refuses it. Nought for every other
        /// format, whose transparent texels are painted and counted.
        uint mPunchThrough;
    };

#ifdef RTX_HOST
    static_assert(sizeof(ShadingSum) == 8, "ShadingSum must be scalar-packed on every side");
    static_assert(sizeof(ShadingConstants) == 12, "ShadingConstants must be scalar-packed on every side");
}
#endif

#endif
