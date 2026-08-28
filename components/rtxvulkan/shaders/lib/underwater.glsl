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

#include "colour.h"
#include "scene.h"
#include "bindings.glsl"
#include "sea.glsl"
#include "traversal.glsl"

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

    const SunUnderWater sun = sunUnderWater(toward);
    const float path = depth * sun.mSlant;

    // **Where the light met the surface, which is up-sun of where it landed.** `path` is how far it
    // came along `mTravelling` to get here, so walking that back along the same line is the point
    // whose curvature focused it — and the whole of what makes a caustic move with the depth and
    // with the light rather than sitting still under the bed.
    return exp(-WATER_EXTINCTION * path) * caustic(position.xy - sun.mTravelling.xy * path, depth, footprint);
}

vec3 waterTransmittance(float path)
{
    return exp(-WATER_EXTINCTION * path);
}

/// What a stretch of water sends toward whoever is looking down it.
///
/// **The sky's half is integrated, and the sun's is too everywhere a shaft would not show.** Water is
/// one density everywhere, so a stretch of it has a closed form where the air — which thins with
/// height and drifts — has only a march. That is what lets the same arithmetic be afforded on the
/// eye's own ray, on a reflection and on a refraction alike. Only inside a narrow cone about the
/// sun's own line is anything marched, and there it is because a shaft has structure a closed form
/// cannot hold.
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
/// **And the sun's half is marched where a shaft would be seen, because a shaft is a caustic.** A
/// beam of sunlight in water is the surface's own lens pattern carried along the ray: the closed
/// form gives the beam's *body* and says nothing about its structure, and the structure is the whole
/// of what makes it read as light through water rather than as haze. Every step takes the same
/// `caustic` a submerged surface takes, at its own depth and its own point of entry — which is why
/// the pattern leans down-sun as it descends instead of standing as a column.
///
/// **And it asks whether the sun reaches that point of entry at all**, which the closed form has no
/// way to. A submerged surface is shadowed because `shadeSurface` traces its own ray and water
/// carries a mask bit that keeps it out of occlusion — so a rock over the sea darkened the bed under
/// it and left the water in front of the bed as bright as ever. The ray goes from where the light
/// met the surface, which the march has already worked out to read the lens at.
///
/// **Only where the beam is a real share of what the stretch sends**, which is `WATER_SHAFT_FLOOR`.
/// Everywhere else the closed form is the whole answer and nothing is marched.
///
/// **And the water over the stretch, which is no part of the stretch.** `(1 - T^2) / 2` counts what
/// the stretch itself crosses and says nothing about what stands above where it begins. From above
/// there is nothing there — the stretch begins at the surface. From below it is the whole column
/// over the camera, and leaving it out let a sea a thousand units down scatter as brightly as one
/// just under the surface. `daylightReaching` is what a submerged *surface* is already dimmed by,
/// so this is the volume agreeing with the surfaces standing in it, and it is the same factor the
/// sun's half below has carried all along.
///
/// **Kept apart rather than applied**, for the reason `fogAlong` gives: the two halves separate
/// later, because an upscaler demodulates the frame by its albedo and what a path took is not part
/// of one. A caller that wants the single number has `throughWater`.
struct WaterColumn
{
    vec3 mTransmittance;
    vec3 mScattered;
};

/// What share of what a stretch of water sends the sun's own beam has to be before its shaft is
/// drawn, and where the shaft reaches full strength.
///
/// **A share and not an angle, which is the same test `fogAlong` makes.** An angle sounds like the
/// right gate — a shaft is the phase function's forward peak — but what decides whether the pattern
/// can be *seen* is the beam against the sky scattered beside it, and that turns with the hour, the
/// weather and the depth. Gated at twenty-six degrees the shafts were there only when the sun was
/// looked straight at; against this they reach as far as they are worth reaching, which at noon in
/// clear water is past forty-five degrees and at dusk further still.
///
/// **Two of them, because one drew a circle.** A march that begins at a threshold begins with a
/// pattern already in it, and the ring where that pattern started was the sharpest edge in the
/// frame. The pattern fades in across the two instead — and the ratio below is what makes *nothing
/// to show* come out as exactly the closed form rather than nearly it.
const float WATER_SHAFT_FLOOR = 0.04;
const float WATER_SHAFT_SHOWN = 0.15;

/// How many samples a shaft is drawn from.
///
/// The pattern varies along the ray at the scale the surface's own lens does, which is why the steps
/// are even rather than bunched: unlike the air, there is no density falling off with height for
/// them to follow, and what wants resolving is spread along the whole stretch.
const uint WATER_SHAFT_STEPS = 8u;

/// @param from where the stretch starts, `direction` the unit direction along it, and `path` how
///        long it is. All three are below the surface.
/// @param footprint how wide the ray's cone is, which is the band limit the caustics are read at.
///        The cone at the far end rather than one per step: the depth's own blur is the larger of
///        the two everywhere a shaft is visible.
/// @param offset where in its first step the march starts, in `[0, 1)`. Without it the samples land
///        on the same shells every frame and the pattern reads as a set of rings.
WaterColumn waterColumn(vec3 from, vec3 direction, float path, float footprint, float offset)
{
    const vec3 transmittance = waterTransmittance(path);
    const vec3 sky = WATER_SCATTER * ((1.0 - transmittance * transmittance) * 0.5) * frame.mAmbient
        * daylightReaching(from);

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

    const float share = brightest(beam) / max(brightest(sky + beam), 1.0e-9);
    if (share < WATER_SHAFT_FLOOR)
        return WaterColumn(transmittance, sky + beam);

    const float show = smoothstep(WATER_SHAFT_FLOOR, WATER_SHAFT_SHOWN, share);

    // **A ratio and not a radiance, which is what makes the march free of its own arithmetic.** The
    // same integrand twice — the sun's own way down, the way back to the eye, and the extinction
    // that is what scattered — once with the surface's lens at every step and once without it. Eight
    // jittered steps are a poor quadrature of either, and an excellent one of what separates them:
    // the step count, the jitter and the exponentials all cancel, and what is left multiplies the
    // closed form above.
    //
    // So a ray that shows no pattern comes back with exactly `beam`, to the last bit, and the ring
    // the gate used to draw has nothing to draw it with.
    vec3 lit = vec3(0.0);
    vec3 plain = vec3(0.0);
    float behind = 0.0;

    for (uint step = 1u; step <= WATER_SHAFT_STEPS; ++step)
    {
        const float ahead = path * float(step) / float(WATER_SHAFT_STEPS);
        const float along = behind + offset * (ahead - behind);

        const vec3 at = from + direction * along;
        const float under = max(frame.mWaterLevel - at.z, 0.0);
        const float reach = under * sun.mSlant;

        const vec3 weight = exp(-WATER_EXTINCTION * (reach + along)) * (ahead - behind);

        // Where the light met the surface, up-sun of where it is scattering — one point, read for
        // the lens that focused it and asked whether anything stood over it.
        const vec2 met = at.xy - sun.mTravelling.xy * reach;

        // **Outside the fade, because a shadow is not fine detail.** `show` brings the *pattern* in
        // across the gate, and a rock's edge has to be there whether or not the filaments are.
        const float visible = lightThrough(vec3(met, frame.mWaterLevel), frame.mSunPosition, frame.mFar);

        lit += weight * mix(1.0, caustic(met, under, footprint), show) * visible;
        plain += weight;
        behind = ahead;
    }

    return WaterColumn(transmittance, sky + beam * (lit / max(plain, vec3(1.0e-20))));
}

/// What is left of `radiance` after a column of water, plus what that column sent back.
vec3 throughWater(vec3 radiance, WaterColumn column)
{
    return radiance * column.mTransmittance + column.mScattered;
}

#endif
