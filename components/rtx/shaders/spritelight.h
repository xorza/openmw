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

    /// Where `spritelight.comp` binds what it reads and writes in set 0, and how many there are.
    /// The shader's layout and the pass's own layout and writes are numbered by these and by
    /// nothing else, so the two cannot drift apart.
    const uint SPRITE_LIGHT_BIND_SOURCE = 0;
    const uint SPRITE_LIGHT_BIND_BAKE = 1;
    const uint SPRITE_LIGHT_BINDINGS = 2;

    /// Lanes along each side of one workgroup, which the kernel declares and the pass divides a
    /// level's extent by.
    const uint SPRITE_LIGHT_WORKGROUP = 16u;

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
