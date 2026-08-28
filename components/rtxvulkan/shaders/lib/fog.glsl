// `#pragma once` everywhere else in this tree, and an include guard here for the reason
// `components/rtx/shaders/portable.h` gives.
#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_FOG_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_FOG_GLSL

// The air between the eye and everything else: how much of it there is at a point, what it
// scatters toward the eye, and what a ray loses crossing it.
//
// **Two elements, and they answer different questions.** The weather's air is Morrowind's own
// record of what the sky is like. The world's edge is this renderer's own, and covers the ring where
// its ground stops.
//
// Both return transmittance and in-scatter apart, so a caller forms `colour * w + xyz` — which is
// what lets fog live here, where the lights already are.

#include "colour.h"
#include "scene.h"
#include "bindings.glsl"
#include "lights.glsl"
#include "random.glsl"
#include "traversal.glsl"
#include "underwater.glsl"

/// The heading and speed each scale drifts on.
///
/// **The differing speeds are what stops it reading as a texture.** One field scrolling rigidly past
/// is a pattern in motion; three shearing against each other at their own rates make the shapes
/// themselves form and pull apart, which is what fog actually does.
const vec2 FOG_CHURN[FOG_SCALES]
    = vec2[FOG_SCALES](vec2(11.0, 7.0), vec2(-6.0, 14.0), vec2(19.0, -4.0));

/// How many shadow rays the sun gets in the fog, and so how many stretches the march is cut into.
///
/// **Not one per step.** A ray costs about four march steps here, so shadowing all twenty-four would
/// cost more than the whole fog does. One ray answers for a stretch, which is what a froxel does
/// too — and the jitter is what keeps that from being a decision always taken in the same place: over
/// frames the probe walks its stretch, so a shaft's edge lands between two neighbours as noise
/// rather than as a step.
///
/// Eight rather than four because they are perfectly coherent — every one of them points at the same
/// sun — so the eighth costs almost nothing. Against a ray-per-step reference the renderer this is
/// ported from measured errors of 0.0155, 0.0134, 0.0087 and 0.0048 for one, two, four and eight.
const uint FOG_SHADOW_RAYS = 8u;

/// Below this share of what the sky puts into the air, the sun does not get a shadow ray.
///
/// **What makes the cost fall only where the shafts are.** Ninety degrees off the sun the phase
/// function is two thousandths of its forward value, so the sun puts less light into the air there
/// than the rounding on the sky's term — and a shaft cut out of light that faint is one nobody can
/// see. Looking away from the sun, and in every interior, this is the whole of what shafts cost.
const float FOG_SHAFT_FLOOR = 0.02;

/// Steps along the view ray.
///
/// **The height falloff is smooth and the lamps are not.** An exponential needs few samples and
/// would take half of these; an inverse square does not, and a step landing beside a lantern reads a
/// spike the two either side of it never see. What that costs is a lamp's halo wobbling as the steps
/// sweep through it while the camera moves — the jitter that arrives with the noise is the answer to
/// it, and until then this count is what keeps it from being obvious.
const uint FOG_STEPS = 24u;

/// How many march steps one shadow ray answers for. `FOG_SHADOW_RAYS` must divide `FOG_STEPS`.
const uint FOG_STEPS_PER_RAY = FOG_STEPS / FOG_SHADOW_RAYS;

/// Where the fog pools when the cell has no water to gather over: sea level outdoors, and close
/// enough to a floor to serve indoors.
const float FOG_BASE = 0.0;

/// The field over a place on the ground, at one scale, read at whatever level the march can tell
/// apart.
///
/// **A level of the chain is the field averaged over twice the texels of the one under it**, so the
/// level a step reaches is the one whose texel is the step's own width. That is the argument
/// `resolved` makes for a wave against a ray cone, and a mip chain makes it exactly, in the sampler,
/// for nothing.
///
/// @param spacing how far apart the march is sampling here.
vec2 fogFieldAt(vec2 position, float tile, float spacing, vec2 churn)
{
    const float texel = tile / float(FOG_FIELD_SIZE);
    const float level = clamp(log2(max(spacing / texel, 1.0)), 0.0, float(FOG_FIELD_LEVELS - 1u));

    return textureLod(fogField, (position + churn * frame.mTime) / tile, level).xy;
}

/// The fog's shape at a point: one volume read at three scales, over a domain the coarsest drags.
///
/// **Fetched rather than computed, which is most of what this stopped costing.** The field this
/// replaced hashed eight lattice corners per octave and took five of those — forty hashes at every
/// step of a twenty-four step march, measured at 2.0 ms of a 2.1 ms trace. Three fetches stand for
/// all of it, and what they read is a richer field than a march could ever have afforded: gradient
/// noise rather than value noise, so no lattice shows as a grid of creases, and four octaves inside
/// the volume before these three scales are laid over each other.
///
/// Its mean is a half and its spread is `FOG_FIELD_SPREAD`, at every level and every distance,
/// which is what the coverage band is cut against.
float fogShape(vec3 position, float spacing)
{
    // **The coarsest scale is read undisplaced.** What a warp is for is breaking the regularity of
    // the structure inside a bank, and at this scale a bank is the whole shape rather than a lattice
    // with something laid on it.
    const vec2 coarse = fogFieldAt(position.xy, FOG_TILE, spacing, FOG_CHURN[0]);

    // Two channels of a fetch already taken, which is what makes a vector out of a scalar field cost
    // nothing at all. Divided by the spread, so what `FOG_WARP` names is a distance rather than a
    // number of standard deviations.
    const vec2 warped = position.xy + (coarse - 0.5) * (FOG_WARP / FOG_FIELD_SPREAD);

    float total = coarse.x - 0.5;
    float squares = 1.0;
    float amplitude = 1.0;
    float tile = FOG_TILE;

    for (uint scale = 1u; scale < FOG_SCALES; ++scale)
    {
        amplitude *= 0.5;
        tile /= FOG_LACUNARITY;

        total += amplitude * (fogFieldAt(warped, tile, spacing, FOG_CHURN[scale]).x - 0.5);
        squares += amplitude * amplitude;
    }

    // **Rescaled by the quadrature sum and not by the plain one**, because the scales are
    // independent draws of one field: a weighted sum of those carries the variance of the weights'
    // squares, so this is what puts the stack back at the spread one scale has. Exact rather than
    // measured, and nothing has to be faded out to hold it there — the level the sampler reached did
    // that already.
    return 0.5 + total / sqrt(squares);
}

/// The height the fog pools at.
///
/// **Measured from the water, not from the origin.** Fog gathers over water and drains off high
/// ground, so the level a cell records is where its layer sits — and above the layer there is none
/// of it, which is what standing on a hill is supposed to look like. A dry cell is handed minus
/// infinity, the same sentinel every other depth question reads, so it falls back to sea level
/// rather than putting the layer infinitely far below the world.
float fogBase()
{
    return isinf(frame.mWaterLevel) ? FOG_BASE : frame.mWaterLevel;
}

/// The fog's extinction at a point, per world unit.
///
/// @param spacing how far apart the march is sampling here, which decides how much of the field it
///        can resolve.
float fogExtinctionAt(vec3 position, float spacing)
{
    // **Air only, and under a bay there is none.** The layer pools *at* the water rather than in
    // it, and a point below the surface already has the water's own absorption over it — fog there
    // would be a second medium laid on the first, putting grey between the eye and the seabed twice
    // over. `mWaterLevel - z` is never positive for a dry cell, so this costs one nothing.
    if (frame.mWaterLevel - position.z > 0.0)
        return 0.0;

    const float height = exp(-max(position.z - fogBase(), 0.0) / FOG_HEIGHT);

    // **Even indoors, and banked out of doors.** Banks are something weather does to a landscape; a
    // room is smaller than one bank and its air is still, so what belongs there is a faint uniform
    // haze rather than a rendering fault. One is what the band averages to, so moving between them
    // changes the air's character and never how much of it there is.
    //
    // **Branched rather than mixed, because `mix` evaluates both sides.** An interior is uniform
    // outright, and a field it then multiplies by nothing was costing it forty hashes a step for an
    // answer it discards — measured at 2.0 ms of a 2.1 ms trace.
    float coverage = 1.0;
    if (frame.mFogUniform < 1.0)
        coverage = mix(smoothstep(FOG_CLEARING, FOG_SOLID, fogShape(position, spacing)) / FOG_COVERAGE, 1.0,
            frame.mFogUniform);

    return frame.mFogExtinction * height * coverage;
}

/// Where along the ray the step ending at `fraction` of the way through reaches.
///
/// **Squared, so the steps bunch where the fog has any shape to it.** Even steps over a ray that can
/// run thirty thousand units give the first hundred a twentieth of one sample and lay the rest
/// across ground too far off to resolve — the same reasoning that makes a froxel grid slice its
/// frustum exponentially rather than evenly.
float fogDepth(float fraction)
{
    return fraction * fraction;
}

/// The mean diameter of the fog's water droplets, in micrometres.
///
/// **The one dial on the shape of the sun's halo.** Radiation fog runs from a few micrometres to
/// about twenty, and the forward peak sharpens brutally with size: at five the fog scatters 1,300
/// times an isotropic one straight down the sun's line, at eight 4,300, at thirty 81,000. Eight is
/// a thick coastal fog.
const float FOG_DROPLET = 8.0;

/// What the fog sends toward the eye per steradian, `cosine` off the sun's line.
///
/// **Mie, not Henyey-Greenstein.** A single lobe is the usual choice and it cannot do this shape:
/// real droplets throw a diffraction peak within a degree of the light that is orders of magnitude
/// above anything one `g` reaches, and they still send a sixth of isotropic *backwards*. Both are
/// what fog looks like — the blaze around a low sun, and fog not going black when you turn away
/// from it. Jendersie and d'Eon fit an HG peak blended with Draine's function to tabulated Mie over
/// droplet diameters of five to fifty micrometres, which is two lobes and four `exp` rather than a
/// table: <https://research.nvidia.com/labs/rtr/approximate-mie/>.
///
/// **Per steradian, and that is not a detail.** The sky needs no phase function at all — it arrives
/// from every direction and a phase function integrates to one over the sphere, so the whole of it
/// scatters in whatever shape the fog has. The sun arrives from one direction as *irradiance*, and
/// what comes back is that irradiance times this. Normalising instead so that isotropic reads one —
/// the convention a lamp's `INV_FOUR_PI` is written in — makes the sun `4 pi` times too bright.
///
/// One evaluation for a whole ray: the sun is directional, so its angle to the view ray is the same
/// at every step, which is the only reason a function of this shape is affordable here.
float fogPhase(float cosine)
{
    const float peak = exp(-0.0990567 / (FOG_DROPLET - 1.67154));
    const float bulk = exp(-2.20679 / (FOG_DROPLET + 3.91029) - 0.428934);
    const float alpha = exp(3.62489 - 8.29288 / (FOG_DROPLET + 5.52825));
    const float share = exp(-0.599085 / (FOG_DROPLET - 0.641583) - 0.665888);

    // Draine's function is Henyey-Greenstein with a `1 + alpha cos^2` term over what that costs it
    // in normalisation.
    const float draine = henyeyGreenstein(bulk, cosine) * (1.0 + alpha * cosine * cosine)
        / (1.0 + alpha * (1.0 + 2.0 * bulk * bulk) / 3.0);

    return mix(henyeyGreenstein(peak, cosine), draine, share);
}

/// How much fog stands between a point of the given `extinction` and the sky along the sun's line.
///
/// **Fog shadows itself, and leaving that out is what makes single scattering white out.** Light
/// reaching a point deep in a bank crossed the whole bank to get there; without this, every point is
/// lit as though it were the first the sun touched — and a phase function that aims the sun at the
/// eye then multiplies something already several times too large.
///
/// Closed form rather than a second march: the density falls off exponentially with height, so the
/// column along a straight line out of it integrates to `sigma * H / cos(zenith)`. Its assumption is
/// that the coverage a point sits in continues along that line, which is what a bank looks like from
/// inside one and is wrong only near an edge, where the fog is thin and the term is near one anyway.
float fogSunDepth(float extinction)
{
    // A sun on the horizon lights an infinite column of fog; the floor is what keeps that finite.
    return extinction * FOG_HEIGHT / max(frame.mSunPosition.z, 1.0e-3);
}


/// What the weather's own air takes out of what is behind it, and what it puts in on the way.
///
/// **Marched rather than integrated.** An exponential falloff with height has a closed form — the
/// whole ray in a handful of instructions — but only while the density is uniform across the
/// horizontal plane, and fog that cannot move is the one thing this is not to be. The march is what
/// the drifting noise costs, paid before there is any.
vec4 fogWeatherAlong(vec3 origin, vec3 direction, float distance, float offset, uint seed)
{
    // No air is the frame untouched, and it has to be exactly that: a lit surface with fog over it
    // is a differently lit one, and the tests that measure radiance turn this off.
    if (!(frame.mFogExtinction > 0.0))
        return vec4(0.0, 0.0, 0.0, 1.0);

    const float span = min(distance, FOG_REACH);

    // One evaluation for the whole march: a directional source holds its angle to the ray, which is
    // what makes a phase function of this shape affordable at all.
    //
    // **Asked once whether there is a sun at all**, the same question `gather` asks before it spends
    // a shadow ray. An interior and a night both answer no, and what they would otherwise pay is an
    // `exp` for the sun's own column at every one of the steps below.
    //
    // **And there is no beam without a sun, which is the same test.** A shaft is the sun seen
    // through air; one with nothing at the end of it lights up the night sky around a sun that is
    // not there. Nothing here has to know what hour it is — `mSunIrradiance` is zero exactly when
    // there is no sun, and it fades to that across dusk rather than stepping.
    const bool sunlit = frame.mSunIrradiance != vec3(0.0);
    const vec3 sunward
        = sunlit ? frame.mSunIrradiance * fogPhase(dot(direction, frame.mSunPosition)) : vec3(0.0);

    // **Only where a shaft could be seen.** The gate is what keeps the rays off every interior and
    // off everything but the sunward part of an exterior, which is most of a frame.
    const bool shafts = brightest(sunward) > FOG_SHAFT_FLOOR * brightest(frame.mFogColour);

    float transmittance = 1.0;
    vec3 scattered = vec3(0.0);
    float behind = 0.0;

    // **One reservoir for the whole march, and so one ray for every lamp at every step of it.** A
    // ray per step per lamp is the cost that kept the air unshadowed, and a ray per stretch — what
    // the sun gets above — is affordable only because the sun's eight all point the same way. A
    // lamp's do not. So every step's lamps are weighed into one reservoir by what that step is worth
    // to the frame, one of them is held, and the single ray it buys stands for all of it.
    //
    // **Isotropic, and `INV_FOUR_PI` is what isotropic is.** A lamp arrives in the air as irradiance
    // exactly as it arrives at a surface, and what comes back toward the eye is that irradiance
    // spread over the sphere — so a lamp with no phase function still owes the factor. Not the real
    // one, either: a lamp's angle to the view ray changes at every step and for every lamp, where a
    // directional source's is fixed for a whole march, and a forward peak thousands of times
    // isotropic would be a firefly waiting for a step to land on the line from the eye through a
    // lantern.
    uint lampState = randomSeed(seed + SEED_LAMPS_FOG);
    Reservoir lamps = noLamps();

    for (uint stretch = 0u; stretch < FOG_SHADOW_RAYS; ++stretch)
    {
        // One ray for the whole stretch, from a point drawn anywhere along it. Holding an answer
        // across several steps is what a froxel does too; drawing where it is taken from the same
        // jitter the steps use is what stops the choice being made in one fixed place every frame.
        float visible = 1.0;
        if (shafts)
        {
            const float reach = fogDepth(float((stretch + 1u) * FOG_STEPS_PER_RAY) / float(FOG_STEPS)) * span;
            const vec3 probe = origin + direction * mix(behind, reach, offset);

            visible = lightThrough(probe, frame.mSunPosition, frame.mFar);
        }

        for (uint k = 0u; k < FOG_STEPS_PER_RAY; ++k)
        {
            const uint i = stretch * FOG_STEPS_PER_RAY + k + 1u;
            const float ahead = fogDepth(float(i) / float(FOG_STEPS)) * span;
            const float stride = ahead - behind;

            // **A different place in every step for every pixel**, so what would be twenty-four
            // visible shells becomes noise a temporal filter can take out. A fixed set of steps
            // lands on the same places every frame otherwise, and a lantern's halo wobbles as they
            // sweep through its falloff.
            const vec3 position = origin + direction * (behind + offset * stride);
            const float extinction = fogExtinctionAt(position, stride);
            behind = ahead;

            // Everything between the sun and this point: what the geometry stopped, what the fog
            // took on the way down, and what any water overhead took out of it.
            const vec3 sun = sunlit
                ? sunward * visible * exp(-fogSunDepth(extinction)) * daylightReaching(position)
                : vec3(0.0);

            // What this step is worth to the frame, computed once and used twice: what it scatters
            // in is weighted by it, and what the transmittance loses to it is exactly it, since
            // `T * (1 - absorbed)` is `T - T * absorbed`.
            const float weight = transmittance * (1.0 - exp(-extinction * stride));

            // **Skipping the lamps where that weight is negligible was measured and is not here.**
            // Air above the layer and air behind fog already opaque both look like free steps to
            // drop, and dropping them bought 3% on Balmora and nothing at all in an interior: at
            // this layer's scale height there is no thin fraction of the ray to skip.
            scattered += weight * (frame.mFogColour + sun);
            weighLamps(lamps, lampState, position, vec3(0.0), INV_FOUR_PI * weight);
            transmittance -= weight;
        }
    }

    // The one ray the march bought, and what every lamp it weighed comes to through it.
    scattered += lampsThrough(lamps, vec2(randomNext(lampState), randomNext(lampState)));

    return vec4(scattered, transmittance);
}

/// What the far end of the world takes out of what is behind it, and what it puts in on the way.
///
/// **The second element of the air, and it is about this renderer rather than about the weather.**
/// The ground stops at `mFogEdge` and the ring where it stops is a cut edge in mid-air. The
/// weather's own extinction cannot close it — it is Morrowind's record of what the air is like, it
/// is measured over that same reach, and clear weather leaves a third of the last cell showing.
/// What closes it is air that is nothing where the player stands and total at the last cell, which
/// is an exponential in the range from the eye.
///
/// **Closed form, because there is nothing along this ray to sample.** The density is a function of
/// the range from the eye alone — no noise, no height, no lamps — so the optical depth is its
/// integral and not a march. Uniform, which is what makes it one.
///
/// **The range is the ray's own and not its shadow on the ground**, because that is how the terrain
/// itself is culled — `distantLandReach` says the rest. Measured flat instead, an eye on a mountain
/// looking down at the ring covers the ground more slowly than it covers distance, so the air never
/// closes and the cut is visible from exactly the places that can see furthest.
///
/// **And it scatters the sky's own gradient rather than the fog's colour.** They are the same thing
/// at the horizon — Morrowind records one colour for both — so nothing is lost near the ring, and
/// above it the gradient is what a ray that reaches nothing already comes back with. So this term
/// converges the world's edge onto exactly the sky beside it, and leaves that sky where it was
/// instead of flattening its lower half toward the horizon.
vec4 fogEdgeAlong(vec3 origin, vec3 direction, float distance)
{
    // A room has no edge to hide, and neither has a test that did not ask for one.
    if (!(frame.mFogEdge > 0.0))
        return vec4(0.0, 0.0, 0.0, 1.0);

    // **Air only, the same test `fogExtinctionAt` makes.** Under a bay the water's own absorption
    // has already closed everything this would, and a second medium over it puts the sky's colour
    // between the eye and the seabed.
    //
    // **The eye alone, where the march tests every step**, because a closed form cannot stop at the
    // surface. So a ray aimed from the air into water is charged for the wet part of its path too —
    // which is worth nothing, since anything deep enough for that to matter is already behind more
    // water than this would ever take.
    if (frame.mWaterLevel - origin.z > 0.0)
        return vec4(0.0, 0.0, 0.0, 1.0);

    // **A climb and not a descent.** Everything above the eye is sky however far off it is, and sky
    // needs no hiding; everything below it is ground, and the ring where that ground stops is the
    // whole reason this is here. An eye on a mountain looks *down* at that ring, so a mask that read
    // the elevation either way would switch the air off in the one place that can see the cut best.
    const float rise = 1.0 - smoothstep(0.0, FOG_EDGE_RISE, max(direction.z, 0.0));
    if (!(rise > 0.0))
        return vec4(0.0, 0.0, 0.0, 1.0);

    // Clamped at the reach, since past it there is no more world to hide and a sky ray carries
    // `mFar` rather than a distance to anything.
    const float range = min(distance, frame.mFogEdge) / frame.mFogEdge;

    // The integral of `exp(range / FOG_EDGE_RAMP)`, normalised to one where the ground stops, so a
    // ray that ends short of the edge is charged for exactly the part of the ramp it crossed.
    const float crossed = (exp(range / FOG_EDGE_RAMP) - 1.0) / (exp(1.0 / FOG_EDGE_RAMP) - 1.0);

    const float transmittance = pow(FOG_EDGE_TRANSMITTANCE, rise * crossed);
    const vec3 haze = skyGradient(frame.mSkyHorizon, frame.mSkyZenith, direction);

    return vec4(haze * (1.0 - transmittance), transmittance);
}

/// The air between the eye and everything else: the weather's, and the world's own edge beyond it.
///
/// Returns the transmittance in `w` and what scattered in along the way in `xyz`, so a caller forms
/// `colour * w + xyz`. Kept apart rather than applied because the two halves separate later — a
/// denoiser demodulates by albedo — and
///
///   `(emitted + albedo * lighting) * T + inscatter == (emitted * T + inscatter) + albedo * (lighting * T)`
///
/// so fogging each half is the same as fogging their sum. That identity is what lets fog live here,
/// where the lights already are, instead of in a pass that would have to bind them all again.
///
/// **The edge stands beyond the weather and not in front of it**, which is where its air actually
/// is: its density is nothing until the last quarter of the reach, so what it scatters has the
/// whole of the weather's air in front of it and arrives dimmed by exactly that.
vec4 fogAlong(vec3 origin, vec3 direction, float distance, float offset, uint seed)
{
    const vec4 weather = fogWeatherAlong(origin, direction, distance, offset, seed);
    const vec4 edge = fogEdgeAlong(origin, direction, distance);

    return vec4(weather.xyz + weather.w * edge.xyz, weather.w * edge.w);
}

#endif
