// `#pragma once` everywhere else in this tree, and an include guard here for the reason
// `components/rtx/shaders/portable.h` gives.
#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_FOG_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_FOG_GLSL

// The air between the eye and everything else: how much of it there is at a point, what it
// scatters toward the eye, and what a ray loses crossing it.
//
// Marched rather than integrated, because fog that cannot move is the one thing this is not
// to be. Returns transmittance and in-scatter apart, so a caller forms `colour * w + xyz` —
// which is what lets fog live here, where the lights already are.

#include "colour.h"
#include "scene.h"
#include "bindings.glsl"
#include "footprint.glsl"
#include "lights.glsl"
#include "random.glsl"
#include "traversal.glsl"
#include "underwater.glsl"

/// How large one cell of the coarsest drift noise is, in world units.
///
/// **Coarse, which is the opposite of the instinct.** Twenty-four steps over a ray that can run
/// thirty thousand units puts more than a thousand between samples at the far end, so structure
/// finer than that never gets sampled twice and arrives as noise rather than as shape. What reads
/// as a bank of fog is the *coarsest* octave; the fine ones only keep its edge from being a circle.
const float FOG_GRAIN = 900.0;

/// Octaves, and the heading and speed each one drifts on.
///
/// **The differing speeds are what stops it reading as a texture.** One field scrolling rigidly past
/// is a pattern in motion; three shearing against each other at their own rates make the shapes
/// themselves form and pull apart, which is what fog actually does. The third carries a little
/// vertical drift so banks rise and settle rather than only sliding.
const int FOG_OCTAVES = 3;
const vec3 FOG_CHURN[FOG_OCTAVES] = vec3[FOG_OCTAVES](vec3(11.0, 7.0, 0.0), vec3(-6.0, 14.0, 2.5),
    vec3(19.0, -4.0, -1.5));

/// Frequency step between octaves. Not two, so the lattices never line up and repeat.
const float FOG_LACUNARITY = 2.27;

/// The spread of the full stack, as a share of one octave's: `sum(a^2) / sum(a)^2` for amplitudes
/// 1, 1/2 and 1/4, which is three sevenths.
///
/// **What the field is rescaled to hold when an octave drops out.** Fewer octaves is a *wider*
/// distribution, not a narrower one — one octave alone has more than twice the variance of three
/// averaged — so a field that simply lost its fine detail would present a different spread to the
/// coverage band, cut a different share of the volume, and quietly stop averaging to `FOG_COVERAGE`.
const float FOG_SPREAD = 3.0 / 7.0;

/// How far the field drags itself sideways before it is sampled, in world units.
///
/// **Domain warping**: rather than adding octaves, the *coordinate* is displaced by a noise of its
/// own, so shapes stretch and curl instead of staying the roughly round blobs a sum of octaves
/// gives. Quilez's `fbm(p + w * fbm(p))` at one level with a single-octave warp — the full
/// construction is an fbm per component per level, which at twenty-four samples a pixel is a
/// different budget from this. Horizontal only: the vertical shape of this fog is the height
/// falloff, and warping across it would blur the layer it is meant to have.
const float FOG_WARP = 450.0;

/// Below `FOG_CLEARING` of the field the air is clear, and at `FOG_SOLID` the fog is at full
/// thickness. Between them it is a bank's edge.
///
/// **This is what makes fog patchy rather than merely uneven.** Scaling density by a noise gives fog
/// that is everywhere and varies; cutting a band out of one gives banks with gaps between them,
/// which is what a valley at dawn looks like.
///
/// **The band has to be measured against the field's own spread, not picked.** Averaging octaves
/// narrows a distribution sharply, and a threshold chosen for one octave's range clears almost
/// everything: the renderer this is ported from tried `0.42..1.0` and left average coverage at a
/// third of a per cent. This field runs mean 0.4996 with a standard deviation of 0.1204, and the
/// band below leaves 40% of the volume clear and 18% at full thickness.
///
/// **Sample it over a plane wider than the grain, not over a sphere.** A million pixels of a sphere
/// of radius 5,000 is a million samples of about 390 cells, and the mean it gives is wrong by
/// several per cent while looking precise. A lattice at 700 units against a 900-unit grain is most
/// of a million independent samples, and the figures here carry a standard error of 0.14%.
const float FOG_CLEARING = 0.45;
const float FOG_SOLID = 0.65;

/// What that band comes to on average, which the coverage is divided by.
///
/// **So the noise redistributes the air rather than removing it.** The extinction the host derived
/// is what a ray should cross on average — it is Morrowind's own view distance, turned into a
/// coefficient — and a band that clears a third of the volume would silently make the world three
/// times clearer than the game says. Normalised, a bank is 2.9 times the derived extinction against
/// a gap of nothing, and the average is what it was.
///
/// **Measured, and it must be re-measured if the band moves.** `theBankedFieldHoldsAsMuchAirAsAnEven
/// One` is what enforces that rather than anyone remembering to.
const float FOG_COVERAGE = 0.3756;

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

/// Trilinear value noise, which is as much structure as a drifting haze needs.
float fogNoise(vec3 at)
{
    const ivec3 base = ivec3(floor(at));

    // Smoothstepped, so the lattice does not show as a grid of creases.
    vec3 fraction = fract(at);
    fraction = fraction * fraction * (3.0 - 2.0 * fraction);

    float total = 0.0;
    for (int corner = 0; corner < 8; ++corner)
    {
        const ivec3 offset = ivec3(corner & 1, (corner >> 1) & 1, (corner >> 2) & 1);
        const vec3 weight = mix(1.0 - fraction, fraction, vec3(offset));
        total += hashToUnit(base + offset) * weight.x * weight.y * weight.z;
    }

    return total;
}

/// The fog's shape at a point: three octaves over a domain dragged sideways by a noise of its own.
///
/// Its own mean is a half by construction, and **its spread is far narrower than one octave's**,
/// because averaging octaves narrows a distribution: with amplitudes 1, 1/2 and 1/4 the variance
/// falls to `sum(a^2) / sum(a)^2` — three sevenths — of a single octave's. That is what the coverage
/// band has to be measured against rather than guessed at.
///
/// **An octave finer than the march's own step is aliasing, not detail.** The steps run from about
/// fifty units near the camera to two and a half thousand at the far end of a long ray, so the
/// finest octave — features 175 units across — is sampled properly on one step in twenty-four and
/// turned into noise on the rest. Fading each one out where the spacing outruns it is the same
/// argument `resolved` makes for a wave against a ray cone.
///
/// **It pays only where the ray is long, which is the half worth knowing.** The span is the distance
/// to whatever was hit, so a view down a street stays fully sampled and gains nothing — measured at
/// Balmora, where every ray ends within a couple of thousand units, the fade is worth under 4%. A
/// view across open ground gains a third, and that is the case that was costing the most.
///
/// The coarsest never fades. It is the one that reads as a bank, so there is always a field here and
/// never a division by nothing.
float fogShape(vec3 position, float spacing)
{
    // Two samples of the same field far apart, which is a cheap way to get a vector out of a scalar
    // noise: they are uncorrelated enough to displace with. It fades on its own terms — its features
    // are twice the grain — and unlike an octave it can go without touching the field's spread,
    // because an undisplaced domain is the same field seen from a different place.
    const float warping = resolved(FOG_GRAIN * 2.0, spacing);
    if (warping > 0.0)
    {
        const vec3 coarse = position / (FOG_GRAIN * 2.0);
        const vec2 warp = vec2(fogNoise(coarse), fogNoise(coarse + vec3(5.2, 1.3, 7.1))) - 0.5;
        position.xy += warp * (FOG_WARP * warping);
    }

    float total = 0.0;
    float weight = 0.0;
    float squares = 0.0;
    float amplitude = 1.0;
    float frequency = 1.0;
    for (int octave = 0; octave < FOG_OCTAVES; ++octave)
    {
        const float held = octave == 0 ? 1.0 : resolved(FOG_GRAIN / frequency, spacing);
        const float used = amplitude * held;
        if (used > 0.0)
            total += used * fogNoise((position * frequency + FOG_CHURN[octave] * frame.mTime) / FOG_GRAIN);

        weight += used;
        squares += used * used;
        amplitude *= 0.5;
        frequency *= FOG_LACUNARITY;
    }

    // Rescaled about the mean, which every octave shares, so that whatever survives presents the
    // spread the whole stack would have. The band above is cut against that spread and nothing else.
    return 0.5 + (total - 0.5 * weight) * sqrt(FOG_SPREAD / squares);
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

/// Henyey-Greenstein, normalised to integrate to one over the sphere.
float henyeyGreenstein(float g, float cosine)
{
    const float squared = g * g;
    const float denominator = 1.0 + squared - 2.0 * g * cosine;

    return INV_FOUR_PI * (1.0 - squared) / (denominator * sqrt(denominator));
}

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

/// What the air at a point sends toward the eye, per unit of light it takes out of the beam.
///
/// **Every lamp that reaches it, so a lantern is a halo in the murk rather than a light with a dark
/// sphere around it.** Unshadowed: a shaft wants a ray per light per step, which is a different
/// order of cost and belongs with the sun's.
///
/// **Isotropic, and `INV_FOUR_PI` is what isotropic is.** A lamp arrives here as irradiance exactly
/// as it arrives at a surface, and what comes back toward the eye is that irradiance spread over the
/// sphere — so a lamp with no phase function still owes the factor. Not the real one, either: a
/// lamp's angle to the view ray changes at every step and for every lamp, where a directional
/// source's is fixed for a whole march, and a forward peak thousands of times isotropic would be a
/// firefly waiting for a step to land on the line from the eye through a lantern.
///
/// `lampsAt` is `gather` without its cosine or its shadow ray — the air has no normal to face away
/// from, and nothing here is occluded. What the two may not disagree on is `falloff`, which is the
/// whole of what a lamp delivers at a distance, and `lampAt` is where that is said.
///
/// @param sun what the sun puts into the air here, with its phase function, its own column of fog
///        and any water overhead already taken out of it. It arrives whole because its angle to the
///        ray does not change along one, unlike every lamp's.
vec3 fogLight(vec3 position, vec3 sun)
{
    return frame.mFogColour + sun + INV_FOUR_PI * lampsAt(position);
}

/// What `distance` units of air take out of what is behind them, and what they put in on the way.
///
/// **Marched rather than integrated.** An exponential falloff with height has a closed form — the
/// whole ray in a handful of instructions — but only while the density is uniform across the
/// horizontal plane, and fog that cannot move is the one thing this is not to be. The march is what
/// the drifting noise costs, paid before there is any.
///
/// Returns the transmittance in `w` and what scattered in along the way in `xyz`, so a caller forms
/// `colour * w + xyz`. Kept apart rather than applied because the two halves separate later — a
/// denoiser demodulates by albedo — and
///
///   `(emitted + albedo * lighting) * T + inscatter == (emitted * T + inscatter) + albedo * (lighting * T)`
///
/// so fogging each half is the same as fogging their sum. That identity is what lets fog live here,
/// where the lights already are, instead of in a pass that would have to bind them all again.
vec4 fogAlong(vec3 origin, vec3 direction, float distance, float offset)
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

            visible = occluded(probe, frame.mSunPosition, frame.mFar) ? 0.0 : 1.0;
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
            scattered += weight * fogLight(position, sun);
            transmittance -= weight;
        }
    }

    return vec4(scattered, transmittance);
}

#endif
