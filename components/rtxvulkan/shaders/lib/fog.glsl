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
#include "variants.glsl"
#include "lights.glsl"
#include "random.glsl"
#include "traversal.glsl"
#include "underwater.glsl"

/// The heading and speed each scale drifts on.
///
/// **The differing speeds are what stops it reading as a texture.** One field scrolling rigidly past
/// is a pattern in motion; three shearing against each other at their own rates make the shapes
/// themselves form and pull apart, which is what fog actually does. The second and the third carry a
/// little vertical drift, so banks rise and settle rather than only sliding.
const vec3 FOG_CHURN[FOG_SCALES]
    = vec3[FOG_SCALES](vec3(11.0, 7.0, 0.0), vec3(-6.0, 14.0, 2.5), vec3(19.0, -4.0, -1.5));

/// How far each scale's read is turned about the vertical, as a rotation of the ground plane.
///
/// **So that no two scales share a lattice.** A lattice noise has directions in it — its own axes,
/// which is where its features line up — and three scales of one volume read on one frame stack
/// those directions rather than averaging them out. Turned against each other, what one scale
/// draws along an axis the next draws across it. The angles are the two smallest Pythagorean
/// triangles, so the matrices are exact and neither is near a quarter turn of the other: 3-4-5 is
/// thirty-seven degrees and 5-12-13 is sixty-seven.
const mat2 FOG_TURN[FOG_SCALES] = mat2[FOG_SCALES](mat2(1.0, 0.0, 0.0, 1.0), mat2(0.8, 0.6, -0.6, 0.8),
    mat2(0.3846154, 0.9230769, -0.9230769, 0.3846154));

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

/// How many slices of a column one shadow ray answers for. `FOG_SHADOW_RAYS` must divide
/// `FOG_VOLUME_SLICES`.
const uint FOG_VOLUME_SLICES_PER_RAY = FOG_VOLUME_SLICES / FOG_SHADOW_RAYS;

/// Where the fog pools when the cell has no water to gather over: sea level outdoors, and close
/// enough to a floor to serve indoors.
const float FOG_BASE = 0.0;

/// The field at a place, at one scale, read at whatever level the march can tell apart.
///
/// **A level of the chain is the field averaged over twice the texels of the one under it**, so the
/// level a step reaches is the one whose texel is the step's own width. That is the argument
/// `resolved` makes for a wave against a ray cone, and a mip chain makes it exactly, in the sampler,
/// for nothing.
///
/// @param spacing how far apart the march is sampling here.
vec2 fogFieldAt(vec3 position, float tile, float spacing, vec3 churn)
{
    const float texel = tile / float(FOG_FIELD_SIZE);
    const float level = clamp(log2(max(spacing / texel, 1.0)), 0.0, FOG_FIELD_COARSEST);

    return textureLod(fogField, (position + churn * frame.mTime) / tile, level).xy;
}

/// The fog's shape at a point: one volume read at three scales, over a domain the coarsest drags.
///
/// **Fetched rather than computed, which is most of what this stopped costing.** The field this
/// replaced hashed eight lattice corners per octave and took five of those — forty hashes at every
/// step of a twenty-four step march, measured at 2.0 ms of a 2.1 ms trace. Three fetches stand for
/// all of it, and what they read is the same trilinear value noise the reference hashes, drawn once.
///
/// **The three scales are the fractal**, and they are the same three the renderer this is ported
/// from sums over a hash: amplitudes halving, frequencies stepping by `FOG_LACUNARITY`, each on
/// its own drift. The volume under them is one octave and nothing more.
///
/// Its mean is a half and its spread is `FOG_FIELD_SPREAD`, at every level and every distance,
/// which is what the coverage band is cut against.
float fogShape(vec3 position, float spacing)
{
    // **The whole field carried downwind, before the scales are dragged past each other** — one
    // displacement rather than three, because a wind moves the air it is in rather than shearing
    // it. Minus, for the reason a cloud sheet subtracts its own drift: a bank sits at a fixed
    // coordinate in the field, so sampling from further upwind as the clock runs is what carries it
    // past, and adding would walk the whole field into the wind.
    position.xy -= frame.mFogWind * (frame.mTime * FOG_GALE);

    // **The coarsest scale is read undisplaced.** What a warp is for is breaking the regularity of
    // the structure inside a bank, and at this scale a bank is the whole shape rather than a lattice
    // with something laid on it.
    const vec2 coarse = fogFieldAt(position, FOG_TILE, spacing, FOG_CHURN[0]);

    // Two channels of a fetch already taken, which is what makes a vector out of a scalar field cost
    // nothing at all. Divided by the spread, so what `FOG_WARP` names is a distance rather than a
    // number of standard deviations.
    //
    // **Horizontal, and the volume does not change that.** The vertical shape of this air is the
    // height falloff, and dragging the domain across it would blur the layer it is meant to have.
    const vec3 warped = position + vec3((coarse - 0.5) * (FOG_WARP / FOG_FIELD_SPREAD), 0.0);

    float total = coarse.x - 0.5;
    float squares = 1.0;
    float amplitude = 1.0;
    float tile = FOG_TILE;

    for (uint scale = 1u; scale < FOG_SCALES; ++scale)
    {
        amplitude *= 0.5;
        tile /= FOG_LACUNARITY;

        const vec3 turned = vec3(FOG_TURN[scale] * warped.xy, warped.z);
        total += amplitude * (fogFieldAt(turned, tile, spacing, FOG_CHURN[scale]).x - 0.5);
        squares += amplitude * amplitude;
    }

    // **Rescaled by the quadrature sum and not by the plain one**, because the scales are
    // independent draws of one field: a weighted sum of those carries the variance of the weights'
    // squares, so this is what puts the stack back at the spread one scale has. Exact rather than
    // measured, and nothing has to be faded out to hold it there — the level the sampler reached did
    // that already.
    return 0.5 + total / sqrt(squares);
}

/// Whether this cell has a water surface for the fog to gather over.
///
/// A dry cell is handed minus infinity, the same sentinel every other depth question reads.
bool fogPools()
{
    return HAS_SEA && !isinf(frame.mWaterLevel);
}

/// The height the fog pools at.
///
/// **Measured from the water, not from the origin.** Fog gathers over water and drains off high
/// ground, so the level a cell records is where its layer sits — and above the layer there is none
/// of it, which is what standing on a hill is supposed to look like. A dry cell falls back to sea
/// level rather than putting the layer infinitely far below the world.
float fogBase()
{
    return fogPools() ? frame.mWaterLevel : FOG_BASE;
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
    if (HAS_SEA && frame.mWaterLevel - position.z > 0.0)
        return 0.0;

    // **How deep the layer stands is the weather's and not a constant.** `FOG_HEIGHT` is the bank
    // clear weather makes in dead still air, and `mFogLift` is what every other weather does to it.
    const float height = exp(-max(position.z - fogBase(), 0.0) / (FOG_HEIGHT * frame.mFogLift));

    // **Even indoors, and banked out of doors.** Banks are something weather does to a landscape; a
    // room is smaller than one bank and its air is still, so what belongs there is a faint uniform
    // haze rather than a rendering fault. One is what the band averages to, so moving between them
    // changes the air's character and never how much of it there is.
    //
    // **Branched rather than mixed, because `mix` evaluates both sides.** An interior is uniform
    // outright, and a field it then multiplies by nothing was costing it forty hashes a step for an
    // answer it discards — measured at 2.0 ms of a 2.1 ms trace.
    //
    // **A far step keeps its banks rather than giving them up for even air.** The two hold the same
    // amount of air on average, so trading one for the other looks free, and it is not: even air is
    // a screen that glows wherever a lamp lights it, where banked air has gaps to see a lit tree
    // through. `FOG_FIELD_COARSEST` is what keeps the far end banked.
    float coverage = 1.0;
    if (!FOG_UNIFORM && frame.mFogUniform < 1.0)
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
/// @param towards unit, from the point toward the light. Each source owes its own slant: at night
///        the sun points down, and a moon standing high crosses far less air than one on the rim.
float fogBeamDepth(float extinction, vec3 towards)
{
    // A source on the horizon lights an infinite column of fog; the floor is what keeps that finite.
    return extinction * FOG_HEIGHT * frame.mFogLift / max(towards.z, 1.0e-3);
}


/// Every directional source over the air, as one ray sees it before anything stands in the way.
///
/// **Hoisted because a directional source holds its angle to the ray**, so its phase function is one
/// evaluation for the whole of it — which is what makes a function of `fogPhase`'s shape affordable
/// at all. A lamp's angle changes at every point, which is why lamps are estimated the other way.
///
/// **Read by the march and by the closed form both**, so what a shaft is cannot come apart between
/// a valley and a room.
struct FogSources
{
    /// Whether there is a sun at all, which an interior and a night both answer no to.
    ///
    /// Nothing here has to know what hour it is — `mSunIrradiance` is zero exactly when there is no
    /// sun, and it fades to that across dusk rather than stepping.
    bool mSunlit;

    /// What it puts into the air along this ray.
    vec3 mSunward;

    /// Whether that is worth a shadow ray.
    ///
    /// **The gate is what keeps the rays off every interior and off everything but the sunward part
    /// of an exterior**, which is most of a frame. And there is no beam without a sun, which is the
    /// same test: a shaft with nothing at the end of it lights up the sky around a sun that is not
    /// there.
    bool mShafts;

    /// **The moons light the air too, and nothing was saying so.** At night the only thing lighting
    /// this haze was `mFogColour`, the dome's own colour — so the air around a moon came back
    /// blue-grey however red the moon, and since the disc itself is dimmed by the air in front of
    /// it, a rainy night drew the fog's colour and none of Masser's.
    vec3 mMasser;
    vec3 mSecunda;

    /// Whether the pair is worth the one ray they share.
    ///
    /// **A moon casts a shaft too, and at night it is the only thing that can.** A headland is not a
    /// penumbra: air standing behind one gets no moonlight at all, and without the test the march
    /// lit the mist in front of a cliff from a moon the cliff was covering.
    bool mMoonlit;

    /// Whichever of the two delivers more, which is what that one ray is aimed at.
    ///
    /// Masser is the larger and the brighter almost always, and a second ray to place Secunda's
    /// shadow separately would cost as much again for a light a quarter its size. The sun's own ray
    /// is not traced at night, so this spends what the day already spends.
    vec3 mMoonward;
};

FogSources fogSourcesAlong(vec3 direction)
{
    const bool sunlit = HAS_SUN && frame.mSunIrradiance != vec3(0.0);
    const vec3 sunward = sunlit ? frame.mSunIrradiance * fogPhase(dot(direction, frame.mSunPosition)) : vec3(0.0);

    const vec3 masser
        = HAS_MOONS ? frame.mMoons[0].mIrradiance * fogPhase(dot(direction, frame.mMoons[0].mDirection)) : vec3(0.0);
    const vec3 secunda
        = HAS_MOONS ? frame.mMoons[1].mIrradiance * fogPhase(dot(direction, frame.mMoons[1].mDirection)) : vec3(0.0);

    const float worthARay = FOG_SHAFT_FLOOR * brightest(frame.mFogColour);

    // **Each flag carries its own constant and not only the terms behind it.** A moon's share folds
    // to nothing without one, but the comparison against a uniform does not fold with it — so the
    // block it guards stays in the kernel, which is the whole of what the constant is for.
    return FogSources(sunlit, sunward, sunlit && brightest(sunward) > worthARay, masser, secunda,
        HAS_MOONS && brightest(masser + secunda) > worthARay,
        brightest(masser) >= brightest(secunda) ? frame.mMoons[0].mDirection : frame.mMoons[1].mDirection);
}

/// What the weather's own air takes out of what is behind it, and what it puts in on the way.
///
/// **Read out of `Rtx::FogVolume` rather than marched.** The field is the same one, the sources are
/// the same and the arithmetic is the one `fogvolume.comp` carries — what changes is that a column
/// of the frustum answers for `FOG_VOLUME_SCALE` squared pixels instead of each of them paying for
/// its own twenty-four steps and its own eight sun probes.
///
/// **The depth is quadratic and so is the fetch.** Slice `k` holds everything up to
/// `fogDepth((k + 1) / FOG_VOLUME_SLICES) * FOG_REACH`, so the coordinate a distance reads at is the
/// inverse of that curve — and the half-texel it is shifted back by is the difference between a
/// slice's far edge, which is where its value stands, and its centre, which is where the sampler
/// thinks it stands.
///
/// **The sun is put back here and not stored.** `fogVolumeSunward` holds the sun's transport with
/// the irradiance and the phase function taken off it, both of which depend on the direction and on
/// nothing along the ray — so the blaze around a low sun keeps the pixel's own angle rather than the
/// column's. `Rtx::FogVolume` says why the moons do not.
vec4 fogVolumeAlong(uvec2 pixel, vec3 direction, float distance)
{
    // The column this pixel stands in, normalised by the image and never by the frame. A traced
    // view is drawn into a volume grown to the largest one asked for, so the two are not the same
    // number — and the pass fills every column the image has for exactly that reason: the pixel at
    // the edge interpolates against the column outside it.
    const vec2 across = (vec2(pixel) + 0.5) / float(FOG_VOLUME_SCALE) / vec2(textureSize(fogVolumeAir, 0).xy);

    const float fraction = sqrt(min(distance, FOG_REACH) / FOG_REACH);
    const vec3 at = vec3(across, fraction - 0.5 / float(FOG_VOLUME_SLICES));

    const vec4 air = texture(fogVolumeAir, at);
    const vec3 sunward = texture(fogVolumeSunward, at).xyz;

    const vec3 sun = HAS_SUN ? sunward * frame.mSunIrradiance * fogPhase(dot(direction, frame.mSunPosition))
                             : vec3(0.0);

    return vec4(air.xyz + sun, air.w);
}

/// How many cells of the light grid one ray may walk before it gives up.
///
/// **A budget and not a limit anything reaches.** The closed form runs where the air is even, which
/// is a room; the grid starts at a cell of one terrain tile, so an interior is a handful of cells
/// across and a ray crosses two or three of them. What this stops is a fine grid under a long ray
/// turning the walk back into the march it replaced.
const uint FOG_CELLS_ALONG = 32u;

/// The fog's optical depth over the first `span` of a ray, exactly.
///
/// **The march exists for the coverage field, and an even haze has none.** With `mFogUniform` at one
/// the only thing left varying along the ray is the height falloff, which is an exponential in `z` —
/// so the integral is the one every layered atmosphere has a closed form for, and twenty-four
/// samples of it are twenty-four samples of a curve two `exp` describe.
///
/// This is `fogExtinctionAt` integrated rather than a second statement of it: below the base the
/// density is the layer's full strength where the cell is dry and nothing at all where the base is
/// the water's own surface, which is what that function says twice over.
float fogColumn(vec3 origin, vec3 direction, float span)
{
    const float base = fogBase();
    const float scale = FOG_HEIGHT * frame.mFogLift;

    // What a point below the base holds. A dry cell's layer is capped there rather than growing
    // without bound; a wet cell's stops at the surface, because the air pools *at* the water.
    const float under = fogPools() ? 0.0 : 1.0;

    const float from = origin.z - base;
    const float to = from + direction.z * span;

    // The stretch spent above the base: the heights it runs between, and its own length.
    float enters = 0.0;
    float leaves = 0.0;
    float above = 0.0;
    if (from > 0.0 && to > 0.0)
    {
        enters = from;
        leaves = to;
        above = span;
    }
    else if (from > 0.0)
    {
        enters = from;
        above = span * (from / (from - to));
    }
    else if (to > 0.0)
    {
        leaves = to;
        above = span * (to / (to - from));
    }

    // **Both exponentials are taken before the division and neither can overflow**, because both
    // heights are above the base. Written the other way round — one `exp` times the mean falloff of
    // the climb — a ray descending a few scale heights asks for `exp` of a large positive number
    // and gets infinity times nothing.
    const float entering = exp(-enters / scale);
    const float leaving = exp(-leaves / scale);
    const float climb = (leaves - enters) / scale;

    // The mean of the falloff over that stretch. A level ray makes that 0/0, and one that climbs
    // less than a ten-thousandth of a scale height loses more of the difference of two exponentials
    // to cancellation than the midpoint of them costs.
    const float mean = abs(climb) < 1.0e-4 ? 0.5 * (entering + leaving) : (entering - leaving) / climb;

    return frame.mFogExtinction * (above * mean + under * (span - above));
}

/// Weighs every lamp along a ray into `kept`, by the light it puts into the air over the whole of it.
///
/// **One walk of the light grid rather than one per step**, which is the cost that made the air
/// expensive in a room: a cell's list is the same list over the whole stretch the ray spends inside
/// it, so it is asked once and each lamp of it is integrated over that stretch. A lamp binned into
/// several cells is weighed once per cell over stretches that do not overlap, which is the same sum
/// the march takes and not a lamp counted twice.
///
/// **The air's own density and what is left of the ray are read at the lamp's closest approach**,
/// held inside the stretch. Everything else about the lamp is integrated; these two vary over a
/// scale height where the inverse square varies over a lamp's reach, so the point that carries
/// nearly all of the share is the one worth reading them at.
void weighLampsAlong(inout Reservoir kept, inout uint state, vec3 origin, vec3 direction, float span)
{
    const float side = 1.0 / grid.mInverseCell;
    const vec3 beyond = grid.mOrigin + vec3(grid.mSize) * side;

    // Clipped to the grid before the walk, so the budget above is spent inside it: a ray that starts
    // outside would otherwise cross empty cells until it ran out.
    float entry = 0.0;
    float exit = span;
    for (int axis = 0; axis < 3; ++axis)
    {
        if (abs(direction[axis]) < 1.0e-8)
        {
            if (origin[axis] < grid.mOrigin[axis] || origin[axis] >= beyond[axis])
                return;
            continue;
        }

        const float one = (grid.mOrigin[axis] - origin[axis]) / direction[axis];
        const float other = (beyond[axis] - origin[axis]) / direction[axis];
        entry = max(entry, min(one, other));
        exit = min(exit, max(one, other));
    }

    if (!(exit > entry))
        return;

    // The cell the ray enters, and for each axis the `t` of its next boundary and the `t` between
    // boundaries after that — a digital differential analyser, so the cell is carried rather than
    // worked out again from a position that would need nudging over each edge.
    vec3 cell = floor((origin + direction * entry - grid.mOrigin) * grid.mInverseCell);
    vec3 next = vec3(exit);
    vec3 stride = vec3(0.0);
    const vec3 onward = sign(direction);

    for (int axis = 0; axis < 3; ++axis)
    {
        if (abs(direction[axis]) < 1.0e-8)
            continue;

        const float boundary = grid.mOrigin[axis] + (cell[axis] + max(onward[axis], 0.0)) * side;
        next[axis] = (boundary - origin[axis]) / direction[axis];
        stride[axis] = side / abs(direction[axis]);
    }

    float behind = entry;
    for (uint visited = 0u; visited < FOG_CELLS_ALONG; ++visited)
    {
        const float leave = min(min(next.x, next.y), next.z);
        const float ahead = min(leave, exit);

        const uvec2 near = lampsWithin(lampsInCell(cell));
        for (uint i = near.x; i < near.y; ++i)
        {
            const GpuLight held = lights[lightIndices[i]];

            const vec3 offset = held.mPosition - origin;
            const float closest = dot(offset, direction);
            const float perpendicular = sqrt(max(dot(offset, offset) - closest * closest, 0.0));

            // The part of this cell's stretch the lamp reaches at all, which is where its chord
            // through the reach and that stretch overlap.
            const float chord = held.mReach * held.mReach - perpendicular * perpendicular;
            if (!(chord > 0.0))
                continue;

            const float halfChord = sqrt(chord);
            const float from = max(behind, closest - halfChord);
            const float to = min(ahead, closest + halfChord);
            if (!(to > from))
                continue;

            // **What the air took is exact, and the falloff is what stands in for a mean.** The
            // light a stretch absorbs is `T(from) - T(to)` and nothing else, because `dT/dt` is
            // `-sigma T` — so the one thing left approximated is that a lamp's falloff and the air's
            // own density do not correlate over the stretch, which they do not: the falloff varies
            // over a lamp's reach and the density over a scale height.
            const float absorbed = exp(-fogColumn(origin, direction, from)) - exp(-fogColumn(origin, direction, to));
            const float share
                = falloffAlong(perpendicular, from - closest, to - closest, held.mReach, held.mSourceRadius)
                / (to - from);

            // Asked of `lampAt` rather than worked out here, so the direction the shadow ray takes
            // and the reach it stops at are the same answer every other consumer of a lamp gets.
            const vec3 place = origin + direction * clamp(closest, from, to);
            const Lamp lamp = lampAt(held, place);

            considerLamp(kept, state, place, lamp.mIntensity * (INV_FOUR_PI * share * absorbed), lamp, false);
        }

        if (leave >= exit)
            return;

        const int axis = next.x <= next.y ? (next.x <= next.z ? 0 : 2) : (next.y <= next.z ? 1 : 2);
        cell[axis] += onward[axis];
        next[axis] += stride[axis];
        behind = leave;
    }
}

/// The weather's own air where the coverage field is even, integrated instead of marched.
///
/// **The march exists to sample two things that vary along the ray, and one of them is gone.** With
/// `mFogUniform` at one the field multiplies by one everywhere, so what is left varying is the
/// height falloff — which has a closed form — and what stands between a point and a light, which
/// does not. So the transmittance is `exp` of the column, the ambient it scatters is
/// `colour * (1 - T)` exactly, every lamp is integrated along the ray in one walk of the grid, and
/// the only loop left is the one the shadow rays were always the whole cost of.
///
/// **A room has a sun and this is what that costs.** Morrowind lights every interior with a
/// directional light — `Sky::roomSun` — so an interior is not the sunless case, and this carries the
/// sun and the moons at the granularity they were already estimated at: one ray a stretch, eight
/// stretches, each weighed by what the air absorbed over exactly that stretch. What went is the
/// twenty-four steps inside them.
///
/// **The one identity the whole thing rests on** is that `dT/dt` is `-sigma T`, so what a stretch
/// takes out of the ray is `T(from) - T(to)` — exactly, with no sampling — and that is the weight
/// every source's share is scaled by.
vec4 fogUniformAlong(vec3 origin, vec3 direction, float distance, float offset, uint seed)
{
    // No air is the frame untouched, and it has to be exactly that: a lit surface with fog over it
    // is a differently lit one, and the tests that measure radiance turn this off.
    if (!(frame.mFogExtinction > 0.0))
        return vec4(0.0, 0.0, 0.0, 1.0);

    const float span = min(distance, FOG_REACH);
    const float transmittance = exp(-fogColumn(origin, direction, span));

    // The ambient, exactly: the source term does not vary along the ray, so the weights a march
    // would have accumulated sum to `1 - T` and to nothing else.
    vec3 scattered = frame.mFogColour * (1.0 - transmittance);

    uint lampState = randomSeed(seed + SEED_LAMPS_FOG);
    Reservoir lamps = noLamps();
    weighLampsAlong(lamps, lampState, origin, direction, span);
    scattered += lampsThrough(lamps, vec2(randomNext(lampState), randomNext(lampState)));

    const FogSources sources = fogSourcesAlong(direction);

    if (sources.mSunlit || sources.mMoonlit)
    {
        float behind = 0.0;
        float leaving = 1.0;

        for (uint stretch = 0u; stretch < FOG_SHADOW_RAYS; ++stretch)
        {
            const float ahead = fogDepth(float(stretch + 1u) / float(FOG_SHADOW_RAYS)) * span;

            // What the air took out of the ray over this stretch, which is the whole of the weight
            // its sources are worth — and it is a difference of two transmittances rather than a
            // sum over steps.
            const float entering = leaving;
            leaving = exp(-fogColumn(origin, direction, ahead));
            const float absorbed = entering - leaving;

            // One point for the stretch, drawn where the march drew it: the fog's own depth toward
            // each source, and the ray that says whether the source is there at all.
            const vec3 probe = origin + direction * mix(behind, ahead, offset);
            const float extinction = fogExtinctionAt(probe, 0.0);
            const vec3 reaching = daylightReaching(probe);
            behind = ahead;

            if (sources.mSunlit)
            {
                const float visible = sources.mShafts ? lightThrough(probe, frame.mSunPosition, frame.mFar) : 1.0;
                scattered += sources.mSunward * (absorbed * visible)
                    * exp(-fogBeamDepth(extinction, frame.mSunPosition)) * reaching;
            }

            if (sources.mMoonlit)
            {
                const float lunar = lightThrough(probe, sources.mMoonward, frame.mFar);
                scattered += (absorbed * lunar)
                    * (sources.mMasser * exp(-fogBeamDepth(extinction, frame.mMoons[0].mDirection))
                        + sources.mSecunda * exp(-fogBeamDepth(extinction, frame.mMoons[1].mDirection)))
                    * reaching;
            }
        }
    }

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
    if (HAS_SEA && frame.mWaterLevel - origin.z > 0.0)
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
vec4 fogAlong(uvec2 pixel, vec3 origin, vec3 direction, float distance, float offset, uint seed)
{
    // **The closed form where the field along the ray is even**, which is a room. The volume is what
    // a coverage field costs, and `fogUniformAlong` is what is left when there is none — so an
    // interior reads no volume at all, and none is dispatched for it.
    vec4 weather;
    if (FOG_UNIFORM)
        weather = fogUniformAlong(origin, direction, distance, offset, seed);
    else
        weather = fogVolumeAlong(pixel, direction, distance);

    const vec4 edge = fogEdgeAlong(origin, direction, distance);

    return vec4(weather.xyz + weather.w * edge.xyz, weather.w * edge.w);
}

#endif
