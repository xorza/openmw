#ifndef OPENMW_COMPONENTS_RTX_SHADERS_RIPPLE_H
#define OPENMW_COMPONENTS_RTX_SHADERS_RIPPLE_H

#include "hosttypes.h"
#include "portable.h"

// What walked through the water: a height field around the eye, stepped by the wave equation and
// stamped where an actor wades, a projectile lands or a spell strikes, and read by the sea as one
// more tile beside its cascades.
//
// **The rasterizer's own simulation, in its own numbers.** `MWRender::RipplesSurface` runs a
// thousand-and-twenty-four-texel field at two and a half units a texel, centred on the player and
// stepped sixty times a second, with `lib/water/ripples.glsl`'s springs. The trace takes the same
// field and reads it as a wave tile: a slope and a curvature with a chain each, so a wake is lit,
// reflected and refracted the way a swell is, and focuses light on the bed under it the way a
// swell does.

#ifdef RTX_HOST
namespace Rtx::Shaders
{
#endif

    /// Where `ripplestep.comp` binds what it reads and writes in set 0, and how many there are. The
    /// shader's layout and the pass's own layout and writes are numbered by these and by nothing
    /// else, so the two cannot drift apart.
    const uint RIPPLE_STEP_BIND_BEFORE = 0;
    const uint RIPPLE_STEP_BIND_AFTER = 1;
    const uint RIPPLE_STEP_BIND_IMPULSES = 2;
    const uint RIPPLE_STEP_BINDINGS = 3;

    /// Where `ripplecompose.comp` binds what it reads and writes in set 0, and how many there are.
    /// The shader's layout and the pass's own layout and writes are numbered by these and by
    /// nothing else, so the two cannot drift apart.
    const uint RIPPLE_COMPOSE_BIND_FIELD = 0;
    const uint RIPPLE_COMPOSE_BIND_SURFACE = 1;
    const uint RIPPLE_COMPOSE_BIND_CURVATURE = 2;
    const uint RIPPLE_COMPOSE_BINDINGS = 3;

    /// Texels along each axis of the field.
    const uint RIPPLE_GRID = 1024u;

    /// World units one texel covers, which with the grid is how far the field reaches: 2560
    /// units, a third of a cell, from the eye in every direction.
    const float RIPPLE_TEXEL = 2.5f;

    /// How often the field is stepped, a second. The step is one texel of neighbourhood, so the
    /// rate is what the springs' wave speed is stated against.
    const float RIPPLE_STEP_RATE = 60.0f;

    /// How many impulses one frame may stamp. Upstream keeps a hundred positions a frame.
    const uint RIPPLE_IMPULSES_MOST = 128u;

    /// The side of the square workgroup every ripple dispatch runs on.
    const uint RIPPLE_WORKGROUP = 16u;

    /// The springs `lib/water/ripples.glsl` states, tuned there by eye to look like water: the
    /// neighbour coupling, and the two dampings on the height and its velocity.
    const float RIPPLE_STIFFNESS = 0.28f;
    const float RIPPLE_DAMPING = 0.04f;

    /// One impulse, in the field's own texels.
    struct GpuRippleImpulse
    {
        /// Where it lands, in texels from the field's origin.
        vec2 mAt;

        /// How wide the stamp is, in texels, and how deep it presses.
        float mRadius;
        float mStrength;
    };

    /// What the step is told: how far the field's window moved since the last step, in texels,
    /// so the old field is read where it now stands, and how many impulses to press on the way out.
    /// The springs and the texel are the constants above, which both kernels read for themselves.
    struct RippleStepConstants
    {
        ivec2 mShift;
        uint mCount;
    };

#ifdef RTX_HOST
    static_assert(sizeof(GpuRippleImpulse) == 16, "GpuRippleImpulse must be scalar-packed on every side");
    static_assert(sizeof(RippleStepConstants) == 12, "RippleStepConstants must be scalar-packed on every side");
}
#endif

#endif
