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

/// Which way the sun travels once it is under the surface, and how far it goes to reach a depth.
///
/// **Refracted at a *flat* surface**, because what the waves do to the direction averages out over
/// a path and what they do to its distribution is the caustic. A low sun is bent hard toward the
/// vertical — Snell's window seen from the light's side — so even a sun on the horizon reaches a
/// point through a path about a third longer than the depth, and never through the infinite one a
/// grazing ray in air would take.
struct SunUnderWater
{
    /// Unit, and pointing down: the way the light goes, not the way the sun lies.
    vec3 mTravelling;

    /// Path length per unit of depth, which is one for a sun overhead.
    float mSlant;
};

SunUnderWater sunUnderWater(vec3 toward)
{
    const vec3 travelling = refract(-toward, vec3(0.0, 0.0, 1.0), 1.0 / WATER_IOR);

    // A tenth of a degree above the horizontal is the floor, which is also where `refract` stops
    // answering: past the critical angle nothing enters the water at all.
    return SunUnderWater(travelling, 1.0 / max(-travelling.z, 0.05));
}

/// What a light in the sky has left, and how it has been gathered, by the time it reaches a point.
///
/// Two things happen to it on the way down. The water absorbs along the path — the *slant* path,
/// which is longer than the depth for any source that is not overhead, and is why a bed is
/// legitimately darker seen from under the water than from above it. And the surface is a lens,
/// which is `caustic`. The shadow ray already passes the surface — water carries a mask bit that
/// keeps it out of occlusion — so this is the whole of what the water does to a light above it.
///
/// **The direction is asked for rather than read off the sun**, because a moon is above the water
/// too and stands somewhere else: a night lit through the sun's slant path is a night lit through a
/// source below the horizon.
///
/// White above the surface, and for a cell with no water at all.
///
/// @param toward unit, from the point to the light.
vec3 lightThroughWater(vec3 position, vec3 toward, float footprint)
{
    const float depth = frame.mWaterLevel - position.z;
    if (!(depth > 0.0))
        return vec3(1.0);

    const float path = depth * sunUnderWater(toward).mSlant;

    return exp(-WATER_EXTINCTION * path) * caustic(position.xy, depth, frame.mTime, footprint);
}

vec3 waterTransmittance(float path)
{
    return exp(-WATER_EXTINCTION * path);
}

/// What a stretch of water sends toward whoever is looking down it.
///
/// **The sky's half and the sun's half, and both are integrated rather than marched.** Water is one
/// density everywhere, so a stretch of it has a closed form where the air — which thins with height
/// and drifts — has only a march. That is the whole reason this costs a handful of instructions
/// against the air's twenty-four steps, and it is why the same arithmetic can be afforded on the
/// eye's own ray, on a reflection and on a refraction alike.
///
/// **The sky, arriving from every direction at once.** A phase function integrates to one over the
/// sphere, so an even sky needs none of it and the whole of what reaches a point scatters. Light
/// that scatters toward the eye had to get down there first: attenuating only the way back — `1 - T`
/// — lets deep water settle at the scattering colour at full sky brightness, which is the milky
/// sheet a real channel is not. Integrating both legs turns that into `(1 - T^2) / 2`, half as
/// bright where it settles and markedly less red, because squaring the transmittance costs red
/// twice over.
///
/// **The sun, arriving along one line, and this is the closed form.** At a point `t` along the ray
/// the sun has crossed `k h(t)` of water to arrive and the scattered light crosses `t` to leave,
/// with `h(t) = h - t d.z` the depth there. Both are exponentials in `t`, so their product is one:
///
///     exp(-o k h) * exp(-o (1 - k d.z) t)
///
/// and the integral over the stretch is `exp(-o k h) (1 - exp(-o g L)) / g` with `g = 1 - k d.z`.
/// **`g` is negative looking up toward the sun**, where a step further along the ray is nearer the
/// surface and better lit — and the product stays bounded anyway, because a ray under the water
/// stops at the surface and `h(L)` never goes below nought.
///
/// **No caustic in it, so there are no shafts yet.** A beam of sunlight in water is the surface's
/// own lens pattern carried down the ray, which is a march. What this gives is the beam's *body*:
/// the water brightening toward the sun and going dark away from it.
///
/// **Kept apart rather than applied**, for the reason `fogAlong` gives: the two halves separate
/// later, because an upscaler demodulates the frame by its albedo and what a path took is not part
/// of one. A caller that wants the single number has `throughWater`.
struct WaterColumn
{
    vec3 mTransmittance;
    vec3 mScattered;
};

/// @param from where the stretch starts, `direction` the unit direction along it, and `path` how
///        long it is. All three are below the surface.
WaterColumn waterColumn(vec3 from, vec3 direction, float path)
{
    const vec3 transmittance = waterTransmittance(path);
    const vec3 sky = WATER_SCATTER * ((1.0 - transmittance * transmittance) * 0.5) * frame.mAmbient;

    // The same test `fogAlong` makes before it spends anything on shafts: an interior and a night
    // both answer no, and `mSunIrradiance` fades to nought across dusk rather than stepping.
    if (frame.mSunIrradiance == vec3(0.0))
        return WaterColumn(transmittance, sky);

    const SunUnderWater sun = sunUnderWater(frame.mSunPosition);

    // Forward is the direction the light was already going, which is `mTravelling`; the eye receives
    // along `-direction`. `fogPhase` measures the same angle in air, where the light travels along
    // `-mSunPosition` and the two spellings agree.
    const vec3 sunward
        = frame.mSunIrradiance * henyeyGreenstein(WATER_ASYMMETRY, -dot(direction, sun.mTravelling));

    const float depth = max(frame.mWaterLevel - from.z, 0.0);
    const float g = 1.0 - sun.mSlant * direction.z;

    // A ray running along the sun's own line has the two exponentials cancel, and the integral is
    // the stretch itself. Written out rather than left to the general form, which divides by `g`.
    const vec3 gathered = abs(g) < 1.0e-3 ? WATER_EXTINCTION * path
                                          : (1.0 - exp(-WATER_EXTINCTION * (g * path))) / g;

    const vec3 beam = WATER_SCATTER * sunward * exp(-WATER_EXTINCTION * (sun.mSlant * depth)) * gathered;

    return WaterColumn(transmittance, sky + beam);
}

/// What is left of `radiance` after a column of water, plus what that column sent back.
vec3 throughWater(vec3 radiance, WaterColumn column)
{
    return radiance * column.mTransmittance + column.mScattered;
}

#endif
