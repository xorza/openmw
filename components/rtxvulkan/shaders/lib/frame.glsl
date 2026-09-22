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

#include "bindings.glsl"
#include "variants.glsl"

/// Whether the sun is over the horizon: lighting, casting, and drawn as a disc.
///
/// `VisibilityConstants::mSun` carries an irradiance that is nought exactly where it is not, and fades to that across
/// dusk rather than stepping, so an interior and a night are the same answer.
bool sunUp()
{
    return HAS_SUN && frame.mSun.mIrradiance != vec3(0.0);
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

/// Whether a ray that found nothing was under the surface looking down, which is water and not sky.
///
/// **The plane has absolute sides**, so below it there is water whether or not this renderer was
/// handed a bed far enough out to stop the ray. Read as sky instead, everything past the edge of
/// the loaded terrain came back at the sky's own horizon colour — which is what `skyGradient`
/// clamps to under the horizontal — through a ray's length of water rather than through the whole
/// of it, and that drew the terrain's boundary across the sea as a row of dark panels. `waterRay`
/// answers the same question the same way for a reflection and for a refraction.
///
/// **Asked by the miss shader and again by the launch**, which are the two that need it: one to
/// draw no sky and one to measure the column the pixel is seen through. It is a plane test and a
/// sign, and a payload word carrying it between them would cost more than asking twice. Here and
/// not with the water's shading, so the miss shader compiles none of that to ask it.
bool waterUnbounded(bool found, vec3 origin, vec3 direction)
{
    return !found && direction.z < 0.0 && waterOver(origin) > 0.0;
}

#endif
