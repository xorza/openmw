// `#pragma once` everywhere else in this tree, and an include guard here for the reason
// `components/rtx/shaders/portable.h` gives.
#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_UNDERWATER_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_UNDERWATER_GLSL

// What a column of water does to the light crossing it.
//
// **Apart from `water.glsl` because the dependency runs both ways otherwise.** A surface
// below the waterline is lit through this, so `gather` needs it — and `shadeWater` needs
// `gather`. The half that answers "what is left of this light" has no opinion about
// shading and comes first; the half that shades a water surface comes after.

#include "scene.h"
#include "bindings.glsl"
#include "sea.glsl"

/// What is left of the daylight by the time it reaches a point, as a fraction per channel.
///
/// The sun and the sky both come from above, so what they lose is the water between the surface and
/// the point they land on. **This was the half that was missing**: absorbing on the way up while
/// lighting the bottom as though the water were not there makes the same column of water read
/// differently from above and below, which is what the invariant test measures.
///
/// White above the surface, and for a cell with no water at all.
vec3 daylightReaching(vec3 position)
{
    const float depth = frame.mWaterLevel - position.z;
    if (!(depth > 0.0))
        return vec3(1.0);

    return exp(-WATER_EXTINCTION * depth);
}

/// What the sun has left, and how it has been gathered, by the time it reaches a point.
///
/// Two things happen to it on the way down. The water absorbs along the path — the *slant* path,
/// which is longer than the depth for any sun that is not overhead, and is why a bed is legitimately
/// darker seen from under the water than from above it. And the surface is a lens, which is
/// `caustic`. The shadow ray already passes the surface — water carries a mask bit that keeps it out
/// of occlusion — so this is the whole of what the water does to sunlight.
///
/// White above the surface, and for a cell with no water at all.
vec3 sunThroughWater(vec3 position, float footprint)
{
    const float depth = frame.mWaterLevel - position.z;
    if (!(depth > 0.0))
        return vec3(1.0);

    // Refracted at a *flat* surface: what the waves do to the sun's direction averages out over the
    // path, and what they do to its distribution is the caustic.
    const vec3 downward = refract(-frame.mSunPosition, vec3(0.0, 0.0, 1.0), 1.0 / WATER_IOR);
    const float path = depth / max(-downward.z, 0.05);

    return exp(-WATER_EXTINCTION * path) * caustic(position.xy, depth, frame.mTime, footprint);
}

/// What is left of `radiance` after `path` units of water, plus what the water scattered back.
///
/// **Light that scatters toward the eye had to get down there first.** Attenuating only the way back
/// — `1 - T` — lets deep water settle at the scattering colour at full sky brightness, which is the
/// milky sheet a real channel is not. Integrating both legs turns that into `(1 - T^2) / 2`: the
/// same answer in the shallows, half as bright where it settles, and markedly less red, because
/// squaring the transmittance costs red twice over.
vec3 waterTransmittance(float path)
{
    return exp(-WATER_EXTINCTION * path);
}

vec3 absorbedByWater(vec3 radiance, float path)
{
    const vec3 transmittance = waterTransmittance(path);
    const vec3 scattered = (1.0 - transmittance * transmittance) * 0.5;

    return radiance * transmittance + WATER_SCATTER * scattered * frame.mAmbient;
}

#endif
