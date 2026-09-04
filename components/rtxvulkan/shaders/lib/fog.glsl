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

#include "camera.h"
#include "colour.h"
#include "fog.h"
#include "look.h"
#include "scene.h"
#include "bindings.glsl"
#include "frame.glsl"
#include "froxel.glsl"
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

// Coverage has no water boundary or height falloff. Those are integrated on the pixel ray.
float fogCoverageAt(vec3 position, float spacing)
{
    if (FOG_UNIFORM || frame.mFogUniform >= 1.0)
        return 1.0;

    return mix(smoothstep(FOG_CLEARING, FOG_SOLID, fogShape(position, spacing)) / FOG_COVERAGE, 1.0,
        frame.mFogUniform);
}

float fogBeamDepthAt(vec3 position, float coverage, vec3 towards)
{
    return fogLightDepth(frame.mFogExtinction * coverage, FOG_HEIGHT * frame.mFogLift,
        position.z - fogBase(), max(towards.z, 1.0e-3));
}

float fogExtinctionAt(vec3 position, float spacing)
{
    if (waterOver(position) > 0.0)
        return 0.0;

    return frame.mFogExtinction * fogCoverageAt(position, spacing)
        * exp(-max(position.z - fogBase(), 0.0) / (FOG_HEIGHT * frame.mFogLift));
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

/// Every directional source over the air, as one ray sees it before anything stands in the way.
///
/// **Hoisted because a directional source holds its angle to the ray**, so its phase function is one
/// evaluation for the whole of it — which is what makes a function of `fogPhase`'s shape affordable
/// at all. A lamp's angle changes at every point, which is why lamps are estimated the other way.
struct FogSources
{
    /// Whether there is a sun at all, which an interior and a night both answer no to.
    ///
    /// Nothing here has to know what hour it is — `mSunIrradiance` is zero exactly when there is no
    /// sun, and it fades to that across dusk rather than stepping.
    bool mSunlit;

    /// What it puts into the air along this ray.
    ///
    /// **Its shadow ray is cast wherever there is a sun**, and not only where this is worth one: the
    /// air throws the sun forward so hard that looking away from it there is nothing to shadow, but a
    /// puff of smoke in the same froxel is lit by the sun at a card's worth whichever way the eye
    /// looks — `puffLight` — and reads the froxel's answer.
    vec3 mSunward;

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
    const bool sunlit = sunUp();
    const vec3 sunward = sunlit ? frame.mSunIrradiance * fogPhase(dot(direction, frame.mSunPosition)) : vec3(0.0);

    const vec3 masser
        = HAS_MOONS ? frame.mMoons[0].mIrradiance * fogPhase(dot(direction, frame.mMoons[0].mDirection)) : vec3(0.0);
    const vec3 secunda
        = HAS_MOONS ? frame.mMoons[1].mIrradiance * fogPhase(dot(direction, frame.mMoons[1].mDirection)) : vec3(0.0);

    const float worthARay = FOG_SHAFT_FLOOR * brightest(frame.mFogColour);

    // **Each flag carries its own constant and not only the terms behind it.** A moon's share folds
    // to nothing without one, but the comparison against a uniform does not fold with it — so the
    // block it guards stays in the kernel, which is the whole of what the constant is for.
    return FogSources(sunlit, sunward, masser, secunda, HAS_MOONS && brightest(masser + secunda) > worthARay,
        brightest(masser) >= brightest(secunda) ? frame.mMoons[0].mDirection : frame.mMoons[1].mDirection);
}

/// Which surfaces end a column's view of the air: everything the eye's own ray stops at, because
/// what the column measures is where the eye's view of the air ends.
const uint FOG_COLUMN_MASK = MASK_SOLID | MASK_WATER | MASK_FIRST_PERSON;

/// The ray through a point `inside` the block of pixels one column of the fog volume stands for,
/// from nought to one across the block.
///
/// From `rayAt`, the same call the trace makes from a pixel, so the two cannot disagree about where
/// a column points. Half a pixel back, because `rayAt` adds its own.
Ray fogColumnRayAt(uvec2 column, vec2 inside)
{
    return rayAt(frame.mCamera, (vec2(column) + inside) * float(FOG_VOLUME_SCALE) - 0.5);
}

/// The ray one column of the fog volume samples its air along this frame.
///
/// **Stated once, because two passes have to agree about it exactly.** `fogdepth.comp` traces it to
/// find where the column's view of the air ends, and `fogscatter.comp` draws every froxel's sample
/// along it — so a froxel's "short of the surface" is measured along the ray the surface was found
/// on.
///
/// **Through a point drawn inside the block, and a different point every frame**, so that over
/// frames the column's froxels cover the block rather than one line through it.
Ray fogColumnRay(uvec2 column)
{
    return fogColumnRayAt(
        column, vec2(randomAt(column + uvec2(17u, 5u), STREAM_FOG), randomAt(column + uvec2(3u, 29u), STREAM_FOG)));
}

vec3 fogDirectionalSegment(FogSources sources, vec4 visibility, float extinction, float scale,
    float from, float to, float depth, float climb)
{
    vec3 result = vec3(0.0);
    if (sources.mSunlit)
    {
        const float rise = max(frame.mSunPosition.z, 1.0e-3);
        result += sources.mSunward * visibility.x * fogLightIntegral(depth,
            fogLightDepth(extinction, scale, from, rise), fogLightDepth(extinction, scale, to, rise), climb / rise);
    }
    if (sources.mMoonlit)
    {
        for (uint moon = 0u; moon < 2u; ++moon)
        {
            const float rise = max(frame.mMoons[moon].mDirection.z, 1.0e-3);
            const vec3 radiance = moon == 0u ? sources.mMasser : sources.mSecunda;
            result += radiance * visibility.w * fogLightIntegral(depth,
                fogLightDepth(extinction, scale, from, rise), fogLightDepth(extinction, scale, to, rise), climb / rise);
        }
    }
    return result;
}

void fogSegment(inout vec4 air, vec2 across, vec3 origin, vec3 direction, float behind, float ahead,
    FogSources sources)
{
    if (!(ahead > behind))
        return;

    const vec3 at = vec3(across, sqrt((0.5 * (behind + ahead)) / FOG_REACH));
    const vec4 point = textureLod(fogSlice, at, 0.0);
    const vec4 visibility = textureLod(fogSliceVisibility, at, 0.0);
    const float extinction = frame.mFogExtinction * point.w;
    const float scale = FOG_HEIGHT * frame.mFogLift;
    const float from = origin.z + direction.z * behind - fogBase();
    const float to = origin.z + direction.z * ahead - fogBase();
    const float depth = fogLayerDepth(extinction, scale, from, to, ahead - behind, false);
    const float absorbed = depth * fogExponentialMean(depth);

    air.xyz += air.w * (absorbed * (frame.mFogColour + point.xyz)
        + fogDirectionalSegment(sources, visibility, extinction, scale, from, to, depth, direction.z));
    air.w *= exp(-depth);
}

// The grid carries coverage and lighting only. Integrating on this ray keeps the known height
// profile and the air/water boundary out of the spatial and temporal filters.
vec4 fogVolumeAlong(uvec2 pixel, vec3 origin, vec3 direction, float distance)
{
    vec4 air = vec4(0.0, 0.0, 0.0, 1.0);
    if (!(frame.mFogExtinction > 0.0))
        return air;

    float reach = min(distance, FOG_REACH);
    if (fogPools() && direction.z < 0.0)
        reach = min(reach, max((frame.mWaterLevel - origin.z) / direction.z, 0.0));

    const vec2 across = (vec2(pixel) + 0.5) / float(FOG_VOLUME_SCALE) / vec2(textureSize(fogSlice, 0).xy);
    const FogSources sources = fogSourcesAlong(direction);
    for (uint slice = 0u; slice < FOG_VOLUME_SLICES; ++slice)
    {
        const float behind = froxelNear(slice);
        if (!(reach > behind))
            break;

        const float middle = min(froxelMiddle(slice), reach);
        const float ahead = min(froxelFar(slice), reach);
        fogSegment(air, across, origin, direction, behind, middle, sources);
        fogSegment(air, across, origin, direction, middle, ahead, sources);
    }
    return air;
}


/// Weighs every lamp reaching a stretch of a ray into `kept`, and returns what they scatter into it.
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
///
/// **The sum and the reservoir are one estimator and not two answers.** Every lamp is offered with
/// the weight of its own share of the sum, so the one held is drawn in proportion to what it
/// contributes — and `sum * lampVisible(kept)` then carries the expectation of the whole sum
/// shadowed lamp by lamp, exactly. What is left of the draw is which lamp the one ray goes to.
///
/// Returns what those lamps scatter toward the eye *integrated over the stretch*: a radiance times
/// a length, so a caller wanting the mean divides by `exit - entry`. `INV_FOUR_PI` is already in
/// it, the air having no side to face a lamp away from.
vec3 lampsInAir(inout Reservoir kept, inout uint state, vec3 origin, vec3 direction, float entry, float exit)
{
    const float side = 1.0 / frame.mLightGrid.mInverseCell;
    const vec3 beyond = frame.mLightGrid.mOrigin + vec3(frame.mLightGrid.mSize) * side;

    vec3 scattered = vec3(0.0);

    // Clipped to the grid before the walk, so the budget above is spent inside it: a ray that starts
    // outside would otherwise cross empty cells until it ran out.
    for (int axis = 0; axis < 3; ++axis)
    {
        if (abs(direction[axis]) < 1.0e-8)
        {
            if (origin[axis] < frame.mLightGrid.mOrigin[axis] || origin[axis] >= beyond[axis])
                return scattered;
            continue;
        }

        const float one = (frame.mLightGrid.mOrigin[axis] - origin[axis]) / direction[axis];
        const float other = (beyond[axis] - origin[axis]) / direction[axis];
        entry = max(entry, min(one, other));
        exit = min(exit, max(one, other));
    }

    if (!(exit > entry))
        return scattered;

    // The cell the ray enters, and for each axis the `t` of its next boundary and the `t` between
    // boundaries after that — a digital differential analyser, so the cell is carried rather than
    // worked out again from a position that would need nudging over each edge.
    vec3 cell = floor((origin + direction * entry - frame.mLightGrid.mOrigin) * frame.mLightGrid.mInverseCell);
    vec3 next = vec3(exit);
    vec3 stride = vec3(0.0);
    const vec3 onward = sign(direction);

    for (int axis = 0; axis < 3; ++axis)
    {
        if (abs(direction[axis]) < 1.0e-8)
            continue;

        const float boundary = frame.mLightGrid.mOrigin[axis] + (cell[axis] + max(onward[axis], 0.0)) * side;
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
            const GpuLight held = lightAt(lightListAt(i));

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

            // **The falloff is what stands in for a mean**, and it is the one thing approximated: a
            // lamp's falloff and the air's own density do not correlate over the stretch, the one
            // varying over a lamp's reach and the other over a scale height. What the air itself
            // takes out of the stretch is not weighed here at all — a froxel hands the integrator a
            // mean over its own length, and the pass that integrates the column applies the air's
            // weight once, afterwards.
            const float crossed
                = falloffAlong(perpendicular, from - closest, to - closest, held.mReach, held.mSourceRadius);

            // Asked of `lampAt` rather than worked out here, so the direction the shadow ray takes
            // and the reach it stops at are the same answer every other consumer of a lamp gets.
            const vec3 place = origin + direction * clamp(closest, from, to);
            const Lamp lamp = lampAt(held, place);

            const vec3 share = lamp.mIntensity * (INV_FOUR_PI * crossed);
            scattered += share;
            considerLamp(kept, state, place, share, lamp);
        }

        if (leave >= exit)
            return scattered;

        const int axis = next.x <= next.y ? (next.x <= next.z ? 0 : 2) : (next.y <= next.z ? 1 : 2);
        cell[axis] += onward[axis];
        next[axis] += stride[axis];
        behind = leave;
    }

    return scattered;
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
vec4 fogAlong(uvec2 pixel, vec3 origin, vec3 direction, float distance)
{
    // A submerged eye is shaded by the water medium up to its first surface.
    if (waterOver(origin) > 0.0)
        return vec4(0.0, 0.0, 0.0, 1.0);

    const vec4 weather = fogVolumeAlong(pixel, origin, direction, distance);
    const vec4 edge = fogEdgeAlong(origin, direction, distance);
    return vec4(weather.xyz + weather.w * edge.xyz, weather.w * edge.w);
}

#endif
