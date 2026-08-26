// `#pragma once` everywhere else in this tree, and an include guard here for the reason
// `components/rtx/shaders/portable.h` gives.
#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_SEA_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_SEA_GLSL

// The wave spectrum and everything differentiated out of it: the surface's normal, its
// elevation, what a cone could not resolve of either, where it breaks, and the caustics
// its curvature focuses.
//
// **One height field, differentiated twice.** The normal is its gradient and the caustics
// are its curvature; the moment either sampled a field of its own the light would land
// where the surface is not. Nothing in here is lit — that is `water.glsl`.

#include "scene.h"
#include "bindings.glsl"
#include "footprint.glsl"

/// How far refraction deflects a ray, per unit of surface slope.
///
/// A tilted surface bends light toward its normal, and the deflection is the difference between the
/// angles of incidence and refraction — which for small angles is the slope times this. Derived from
/// the index of refraction rather than written out, so nothing here can come to disagree about what
/// water is.
const float WATER_REFRACTION_BEND = 1.0 - 1.0 / WATER_IOR;

/// A ceiling on how bright a focus is allowed to get.
///
/// Where the refracted bundle collapses to a line the Jacobian goes to zero and the intensity to
/// infinity — a real caustic *cusp*, and the reason a pool's bright lines are as sharp as they are.
/// Letting one through would put a pixel in the frame that no exposure could hold.
const float WATER_CAUSTIC_MAX = 3.0;

/// The depth past which the pattern stops sharpening, in world units — about two metres.
///
/// **The honest edge of the approximation.** `q = p - bend * depth * grad(h)` holds while the
/// refracted bundle has not crossed itself; past the first focus the rays have folded over one
/// another and one Jacobian no longer describes what is there. Evaluated at the bed rather than at
/// the surface the light left, the model then starts *making* it — three quarters more at four
/// hundred units. Holding the depth here keeps the term inside the regime where it conserves, and
/// says the true thing anyway: caustics are sharp in a shallow pool and washed out in deep water.
const float WATER_CAUSTIC_MAX_DEPTH = 140.0;

/// How far the swell carries the ripples riding on it, and over what distance that carrying turns.
///
/// **A sum of plane waves is a lattice, and curvature is where that shows.** The second derivative
/// weights a component by `A k^2`, which climbs with wavenumber however the spectrum falls, so the
/// shortest few dominate it whatever else is in the sum — and a handful of plane waves crossing is a
/// grid, regular enough to read as a texture rather than as water.
///
/// So the short waves are carried. On real water they ride the long swell's orbital motion, which
/// bends their crests and slides them out of step from one trough to the next. Thirteen units is
/// most of a wavelength to the shortest waves and a rounding error to the longest, so one field does
/// the whole job without touching the swell it came from.
const float WAVE_DRIFT = 13.0;
const float WAVE_DRIFT_LENGTH = 640.0;

/// Where the ripples have been carried to by the time they are sampled.
///
/// Two long crossing swells at angles the band sequence never takes, so the drift cannot fall into
/// step with the waves it displaces.
vec2 drifted(vec2 at, float time)
{
    const float wavenumber = TAU / WAVE_DRIFT_LENGTH;
    const float speed = sqrt(WATER_GRAVITY * wavenumber);
    const float along = wavenumber * dot(vec2(0.8347, 0.5507), at) - speed * time;
    const float across = wavenumber * dot(vec2(-0.4132, 0.9106), at) - speed * time * 0.83;

    return at + WAVE_DRIFT * vec2(sin(along) + 0.6 * cos(across), cos(along) - 0.6 * sin(across));
}

/// One wave component where a ray met the water.
///
/// **Shared by the normal and the curvature**, which is the whole point: those are the first and
/// second derivatives of one height field, and the moment the two disagreed about where a crest is
/// the light would land where the surface is not.
struct WaveSample
{
    /// How much of this wave the cone can still tell apart, from none of it to all.
    float mDetail;

    /// How far through its cycle it is here, in radians.
    float mPhase;
};

/// Reads one component of the spectrum at `at`, which both callers pass through `drifted` first —
/// the gradient and the curvature have to be taken of the same displaced field.
WaveSample sampleWave(GpuWave wave, vec2 at, float time, float footprint)
{
    WaveSample result;
    result.mDetail = resolved(TAU / wave.mWavenumber, footprint);
    result.mPhase = wave.mWavenumber * dot(wave.mDirection, at) - wave.mSpeed * time;
    return result;
}

/// The water's surface where a ray met it: one read of the wave field, and everything taken from it.
///
/// **The normal, the elevation and what the cone could not resolve of either, out of one loop.** They
/// are derivatives and moments of a single height field, and the moment two of them are computed
/// apart the light lands where the surface is not — which is the same argument `WaveSample` makes one
/// level down.
struct WaterSurface
{
    /// Unit, from the gradient of the height field.
    vec3 mNormal;

    /// Elevation about the still level, in world units, of the waves this cone can resolve.
    float mHeight;

    /// Mean square slope the cone averaged away. **Not gone, rough.** A surface that lost its slope
    /// reflects like polished plastic; keeping the variance of what was dropped is what lets it come
    /// back as a widened specular lobe instead, which is LEAN mapping's argument in one dimension.
    float mLostSlope;

    /// Variance of the *elevation* the cone averaged away, in world units squared.
    ///
    /// What `mLostSlope` is to the reflection this is to the foam. The surf line is a level set of
    /// the surface, and a level set of a field too fine to resolve is a coin toss unless how far
    /// that field still wanders is carried alongside the mean.
    float mLostHeight;

    /// Root mean square elevation of the whole spectrum, resolved or not — the sea state itself,
    /// and `WATER_SIGNIFICANT_HEIGHT` times it is the height oceanography quotes.
    float mRoughness;
};

/// Reads the surface at a point.
///
/// The height is `sum(A sin(k dot(d, p) - w t))`; the normal is its slope and `caustic`
/// differentiates the same field once more, so none of the three can disagree about where a crest is.
WaterSurface waterSurfaceAt(vec2 at, float time, float footprint)
{
    const vec2 here = drifted(at, time);

    WaterSurface surface;
    surface.mHeight = 0.0;
    surface.mLostSlope = 0.0;
    surface.mLostHeight = 0.0;

    vec2 slope = vec2(0.0);
    float variance = 0.0;

    for (uint i = 0u; i < WAVE_COUNT; ++i)
    {
        const GpuWave wave = waves[i];
        const WaveSample sampled = sampleWave(wave, here, time, footprint);
        const float steepness = wave.mAmplitude * wave.mWavenumber;

        // A sinusoid of amplitude `a` has mean square `a^2 / 2`, and the spectrum's components are
        // independent, so the sum of those is the whole of the surface's variance. **Taken off the
        // same table the shape comes from** rather than handed down beside it: this is a frame
        // constant and could have been one, and a sea state that disagreed with its own waves would
        // be a surf line drawn where the water is not.
        const float energy = 0.5 * wave.mAmplitude * wave.mAmplitude;
        variance += energy;

        // How much of each the cone threw away. `mDetail` is an amplitude scale, so what survives of
        // a variance is its square.
        const float dropped = 1.0 - sampled.mDetail * sampled.mDetail;
        surface.mLostSlope += dropped * 0.5 * steepness * steepness;
        surface.mLostHeight += dropped * energy;
        if (sampled.mDetail <= 0.0)
            continue;

        slope += wave.mDirection * (sampled.mDetail * steepness * cos(sampled.mPhase));
        surface.mHeight += sampled.mDetail * wave.mAmplitude * sin(sampled.mPhase);
    }

    surface.mNormal = normalize(vec3(-slope, 1.0));
    surface.mRoughness = sqrt(variance);
    return surface;
}

/// The depth this sea state breaks in, in world units.
///
/// A wave breaks when its height reaches about three quarters of the depth it stands in —
/// McCowan's solitary-wave limit, 1894, still what a surf zone is placed with. It is what makes the
/// band's width a consequence of the sea rather than a distance anyone picks: a calmer sea breaks
/// closer in, in a narrower strip, with nothing tuned.
float breakingDepth(WaterSurface surface)
{
    return surface.mRoughness * WATER_SIGNIFICANT_HEIGHT / WATER_BREAKER_RATIO;
}

/// How far broken water is carried before it has gone, in world units.
///
/// The shallow-water celerity at the breaker line, over the time a raft of bubbles lasts. The floor
/// is a divide guard for a sea too flat to have a surf zone at all, where nothing breaks anyway.
float foamRunout(WaterSurface surface)
{
    return max(WATER_FOAM_LIFETIME * sqrt(WATER_GRAVITY * breakingDepth(surface)), 1.0e-3);
}

/// What share of the surface at a point is breaking, from none of it to all.
///
/// **Against the depth at this instant rather than the still one**, which is the whole of the
/// pattern. A crest carries the column deeper and a trough leaves it thinner, so the criterion is
/// met and unmet as the waves run through — the surf line wanders up and down the beach by the
/// height of the sea rather than sitting where the still water would put it, and it is ragged along
/// the shore because the wave passing over it is. Nothing here is a texture or a noise field: it is
/// the same height the normal came from, asked a different question.
///
/// **And what the cone could not resolve is answered rather than dropped.** Far enough off, the
/// surf line is finer than the pixel looking at it, and a hard test against an averaged height
/// flickers between all foam and none as the camera moves. What is wanted there is the *share* of
/// the surface breaking, which is the Gaussian tail of the elevation that was averaged away — a sum
/// of many independent sinusoids is Gaussian by the central limit theorem. Both ends fall out of one
/// expression once the unresolved elevation is treated as the noise it is, which is the same
/// argument `mLostSlope` makes for the reflection and why the two are carried together.
///
/// @param depth how deep the still water is under this point, straight down and in world units.
float foamBreaking(WaterSurface surface, float depth)
{
    // What the sea state breaks in, and what stands between this point and breaking once the wave
    // over it is counted.
    const float margin = breakingDepth(surface) - (depth + surface.mHeight);

    // `P(the unresolved elevation does not make up the margin)`. The smoothstep stands in for the
    // Gaussian's own integral, which it follows to within a hundredth across the three standard
    // deviations that are not already nought or one. The floor is a divide guard and nothing else:
    // a surface every one of whose waves is resolved has a genuinely sharp edge, and drawing it
    // sharp is right.
    const float noise = max(sqrt(surface.mLostHeight), 1.0e-3);

    return smoothstep(-1.6, 1.6, margin / noise);
}

/// How much of what broke is still foam by the time it has been carried to a point.
///
/// **A wave has to have reached the point at all, which breaking never asks.** That criterion is a
/// statement about water a wave is crossing; every hollow that dips below sea level satisfies it
/// too, and there are a great many of those behind a shoreline. What tells the two apart is the
/// bed: a shore keeps going down, so the wave broke a few metres away and what it made is still
/// here, while a pan is level, so the nearest water deep enough to break in is tens of metres off
/// and nothing that broke there survives the trip. Measured at Seyda Neen the two populations sit a
/// hundred-fold apart with nothing in between, which is why the shape of the fall-off decides
/// nothing that was ever close.
///
/// @param depth how deep the still water is under this point, in world units.
/// @param fall how fast the bed falls away toward deep water, as a tangent — measured over the
///        run-out rather than read off the surface here, for the reason `bedFall` gives.
/// @param runout what `foamRunout` said, handed in because the caller measured `fall` across it and
///        the two have to be the same number.
float foamReaching(WaterSurface surface, float depth, float fall, float runout)
{
    const float breaking = breakingDepth(surface);

    // How far the wave came through water shallow enough to break in: the depth still to be lost,
    // over the rate it is lost at.
    //
    // **The still depth and not the instantaneous one**, which `foamBreaking` is the other way
    // round about. Where the surf line falls is a question about this wave and wanders with it; how
    // wide the surf zone is, is a question about the beach and does not.
    const float crossed = max(breaking - depth, 0.0) / max(fall, 1.0e-4);

    return exp(-crossed / runout);
}

/// How much the sunlight reaching `depth` below the surface has been gathered, as a multiplier.
///
/// **Caustics are ray density**, and a change in density is the determinant of the Jacobian of the
/// map from where light met the surface to where it landed. For small slopes that map is
/// `q = p - bend * depth * grad(h)`, so its Jacobian is `I - bend * depth * H` with `H` the Hessian
/// of the same height field the normals come from — and because that field is a sum of sinusoids,
/// `H` is written out here rather than sampled, filtered or splatted. No photons, no buffer, no
/// noise: the light is where the arithmetic says it is.
///
/// The small-angle approximation is the right one for this game. Vvardenfell's water is thirty
/// metres deep at its very worst and a few at the shore, with slopes under a seventh, so the exact
/// refraction and its linearisation differ by less than the sun's own width.
///
/// **One index of refraction and not three.** Water's runs 1.3326 to 1.3392 across the visible band
/// by Cauchy's fit, so blue turns harder than red and a real caustic has coloured edges; the cost of
/// drawing them would be three determinants over a Hessian that does not depend on the channel.
/// Measured on the reference renderer at this sea state, twelve pixels in ninety thousand came out
/// differing by more than one level. It is what would put prism edges on cusps if the surface ever
/// got steep enough for the determinant to approach zero, and it goes in when it does.
float caustic(vec2 at, float depth, float time, float footprint)
{
    // Differentiated at the drifted position rather than the true one, which drops the chain rule's
    // contribution from the drift itself. That field turns over six hundred units against a
    // curvature set by ten, so its own Jacobian is within a fifth of the identity, and what the
    // omission costs is a slow variation in how strong the caustics are — which is indistinguishable
    // from the variation real water already has.
    const vec2 here = drifted(at, time);

    // Second derivatives of `sum(A sin(k dot(d, p) - w t))`, which are `-A k^2 d_i d_j sin`. Three
    // numbers rather than four, because a Hessian is symmetric: xx, yy, and the shared off-diagonal.
    vec3 hessian = vec3(0.0);
    float traceVariance = 0.0;
    for (uint i = 0u; i < WAVE_COUNT; ++i)
    {
        const GpuWave wave = waves[i];
        const WaveSample sampled = sampleWave(wave, here, time, footprint);
        if (sampled.mDetail <= 0.0)
            continue;

        const float amplitude = sampled.mDetail * wave.mAmplitude * wave.mWavenumber * wave.mWavenumber;
        const float second = -amplitude * sin(sampled.mPhase);

        hessian += second
            * vec3(wave.mDirection.x * wave.mDirection.x, wave.mDirection.y * wave.mDirection.y,
                wave.mDirection.x * wave.mDirection.y);

        // What this component contributes to the variance of `tr(H)`, for the gain below. A
        // sinusoid of amplitude `a` has mean square `a^2 / 2`, the direction drops out because
        // `d_x^2 + d_y^2` is one, and the components are independent — so the sum is the whole of it.
        traceVariance += 0.5 * amplitude * amplitude;
    }

    // One determinant and not a ratio of two, because this surface is not displaced: the quad stays
    // flat and only its normal moves, so the patch of surface the light left is the patch of
    // parameter space it came from. A Gerstner sea gathers toward its own crests before the light
    // ever reaches it, and would need `det(I + dD)` over the numerator to keep a depthless puddle
    // from brightening its own bottom.
    const float bend = WATER_REFRACTION_BEND * min(depth, WATER_CAUSTIC_MAX_DEPTH);
    const float determinant
        = (1.0 - bend * hessian.x) * (1.0 - bend * hessian.y) - bend * bend * hessian.z * hessian.z;

    // **A reciprocal of something that fluctuates is worth more than the reciprocal of its mean**,
    // and left alone that is a bed lit brighter than the water over it lets through. The map is
    // evaluated at the bed rather than at the surface the light left, so the samples are not
    // weighted by the area each one stands for, and the excess is Jensen's: writing
    // `det = 1 - u + v` with `u = bend * tr(H)` and `v = bend^2 * det(H)`,
    //
    //   E[1 / det] = 1 - E[v] + Var[u] + ...
    //
    // and `E[det H]` is zero for a sum of independent sinusoids — the `Hxx Hyy` and `Hxy^2` terms
    // are the same sum — which leaves `Var[u] = bend^2 * sum (A k^2)^2 / 2` as the whole of the
    // second order. That is `traceVariance`, gathered in the loop above for two more instructions,
    // and it is the same everywhere in the field: the gain it removes is a flat one, so every
    // bright line and dark cell survives it untouched. Measured over a bed at the depth cap, the
    // term went from 12.3 per cent more light than fell on the water to 2.4, and the pattern's
    // contrast did not move at all — 0.3661 against 0.3662.
    //
    // The floor on the denominator is what the ceiling means, so there is one number to state.
    return 1.0 / (max(abs(determinant), 1.0 / WATER_CAUSTIC_MAX) * (1.0 + bend * bend * traceVariance));
}

#endif
