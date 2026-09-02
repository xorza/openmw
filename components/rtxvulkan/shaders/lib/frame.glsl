// `#pragma once` everywhere else in this tree, and an include guard here for the reason
// `components/rtx/shaders/portable.h` gives.
#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_FRAME_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_FRAME_GLSL

// What this frame says about the world, asked one way.
//
// **Each of these was a test written out in five or six places, and no two of the spellings had
// to agree.** Whether there is a sun was `HAS_SUN && mSunIrradiance != vec3(0.0)` in the shading,
// `!HAS_SUN || mSunIrradiance == vec3(0.0)` in the water and `HAS_SUN && ...` again in the sprites;
// whether a point stands under the water was a subtraction in four files, two of them guarded by
// the sea's constant and two of them not. A frame that changed how it said one of these would have
// had to find every copy. These pair the compile-time constant with the runtime test it stands in
// front of, which is the rule `variants.glsl` states, and every reader asks here.

#include "scene.h"
#include "visibility.h"

#include "bindings.glsl"
#include "variants.glsl"

/// Whether the sun is over the horizon: lighting, casting, and drawn as a disc.
///
/// `VisibilityConstants::mSunIrradiance` is nought exactly where it is not, and fades to that across
/// dusk rather than stepping, so an interior and a night are the same answer.
bool sunUp()
{
    return HAS_SUN && frame.mSunIrradiance != vec3(0.0);
}

/// Whether the sky is a light: the ambient is the sky's and a ray that leaves the world finds it.
///
/// **What `mAmbientFromSky` decides, and it decides three things**: whether a bounce that escapes
/// brings anything back, how far the ambient looks for what occludes it, and whether an indirect
/// hit is rated. `VisibilityConstants::mAmbientFromSky` carries the argument for each.
bool skyLights()
{
    return frame.mAmbientFromSky > 0.0;
}

/// How much water stands over a point, in world units — or nothing at all above the surface, and
/// in a cell that has none.
///
/// **One subtraction, so a dry cell's sentinel is read in one place.** The level is minus infinity
/// where there is no water, which makes the difference never positive — and `HAS_SEA` takes the
/// whole test out of a kernel that was built for a room.
float waterOver(vec3 position)
{
    return HAS_SEA ? max(frame.mWaterLevel - position.z, 0.0) : 0.0;
}

#endif
