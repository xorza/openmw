#ifndef OPENMW_COMPONENTS_RTX_SHADERS_SPRITELIGHT_H
#define OPENMW_COMPONENTS_RTX_SHADERS_SPRITELIGHT_H

#include "hosttypes.h"
#include "portable.h"

// The sprite light bake's dispatch: what `spritelight.comp` is told about the level it bakes.
// `Rtx::SpriteLightMap` says what the bake is and why.

#ifdef RTX_HOST
namespace Rtx::Shaders
{
#endif

    /// One level of the bake, which is one dispatch.
    struct SpriteLightConstants
    {
        /// Which level of the source is read and which of the bake is written.
        uint mLevel;

        uint mWidth;
        uint mHeight;
    };

#ifdef RTX_HOST
    static_assert(sizeof(SpriteLightConstants) == 12, "SpriteLightConstants must be scalar-packed on every side");
}
#endif

#endif
