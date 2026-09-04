#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_FOG_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_FOG_GLSL

//! Analytic air transport through sampled coverage and lighting, followed by the world-edge haze.

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

// Different drifts deform banks instead of translating a rigid texture.
const vec3 FOG_CHURN[FOG_SCALES]
    = vec3[FOG_SCALES](vec3(11.0, 7.0, 0.0), vec3(-6.0, 14.0, 2.5), vec3(19.0, -4.0, -1.5));

// Exact 3-4-5 and 5-12-13 rotations keep the three noise lattices from sharing axes.
const mat2 FOG_TURN[FOG_SCALES] = mat2[FOG_SCALES](mat2(1.0, 0.0, 0.0, 1.0), mat2(0.8, 0.6, -0.6, 0.8),
    mat2(0.3846154, 0.9230769, -0.9230769, 0.3846154));

/// Samples the noise mip whose texel size matches the integration spacing.
vec2 fogFieldAt(vec3 position, float tile, float spacing, vec3 churn)
{
    const float texel = tile / float(FOG_FIELD_SIZE);
    const float level = clamp(log2(max(spacing / texel, 1.0)), 0.0, FOG_FIELD_COARSEST);

    return textureLod(fogField, (position + churn * frame.mTime) / tile, level).xy;
}

/// Three independent noise scales over a domain displaced by the coarsest.
float fogShape(vec3 position, float spacing)
{
    // Sampling upwind carries a fixed bank downwind as time advances.
    position.xy -= frame.mFogWind * (frame.mTime * FOG_GALE);

    const vec2 coarse = fogFieldAt(position, FOG_TILE, spacing, FOG_CHURN[0]);

    // Horizontal warping preserves the analytic height profile.
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

    // Independent scales add variances, so normalize by the quadrature sum of their weights.
    return 0.5 + total / sqrt(squares);
}

// A dry cell carries minus infinity, matching the water-depth queries.
bool fogPools()
{
    return HAS_SEA && !isinf(frame.mWaterLevel);
}

// Wet cells measure the layer from their water surface; dry cells use sea level.
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

/// Droplet phase per steradian, at the cosine between view and light directions.
// The HG-Draine fit preserves both the narrow forward diffraction peak and backward scattering.
// https://research.nvidia.com/labs/rtr/approximate-mie/
// Directional irradiance needs this phase; the ambient already integrates over incoming directions.
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

// Phase is constant along a pixel ray and belongs outside its integration loop.
struct FogSources
{
    bool mSunlit;
    vec3 mSunward;
    vec3 mMasser;
    vec3 mSecunda;
    bool mMoonlit;

    // The moons share one shadow ray, aimed toward the brighter contribution.
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

    // Include specialization constants in the flags so absent light paths compile away.
    return FogSources(sunlit, sunward, masser, secunda, HAS_MOONS && brightest(masser + secunda) > worthARay,
        brightest(masser) >= brightest(secunda) ? frame.mMoons[0].mDirection : frame.mMoons[1].mDirection);
}

float fogDirectionalIntegral(float depth, vec2 column, float climb, vec3 towards)
{
    const float slant = 1.0 / max(towards.z, 1.0e-3);
    return fogLightIntegral(depth, column.x * slant, column.y * slant, climb * slant);
}

vec3 fogDirectionalSegment(FogSources sources, vec2 visibility, float extinction, float scale,
    float from, float to, float depth, float climb)
{
    if (!sources.mSunlit && !sources.mMoonlit)
        return vec3(0.0);

    const vec2 column = vec2(fogLightDepth(extinction, scale, from, 1.0),
        fogLightDepth(extinction, scale, to, 1.0));
    vec3 result = vec3(0.0);
    if (sources.mSunlit)
        result += sources.mSunward * visibility.x
            * fogDirectionalIntegral(depth, column, climb, frame.mSunPosition);
    if (sources.mMoonlit)
    {
        for (uint moon = 0u; moon < 2u; ++moon)
        {
            const vec3 radiance = moon == 0u ? sources.mMasser : sources.mSecunda;
            result += radiance * visibility.y
                * fogDirectionalIntegral(depth, column, climb, frame.mMoons[moon].mDirection);
        }
    }
    return result;
}

struct FogRay
{
    vec2 mAcross;
    float mHeight;
    float mClimb;
    float mScale;
    FogSources mSources;
};

void fogSegment(inout vec4 air, FogRay ray, float behind, float ahead)
{
    if (!(ahead > behind))
        return;

    const vec3 at = vec3(ray.mAcross, fogVolumeDepth(0.5 * (behind + ahead)));
    const vec4 point = textureLod(fogSlice, at, 0.0);
    const vec2 visibility = textureLod(fogSliceVisibility, at, 0.0).xy;
    const float extinction = frame.mFogExtinction * point.w;
    const float from = ray.mHeight + ray.mClimb * behind;
    const float to = ray.mHeight + ray.mClimb * ahead;
    const float depth = fogLayerDepth(extinction, ray.mScale, from, to, ahead - behind, false);
    const float absorbed = depth * fogExponentialMean(depth);

    air.xyz += air.w * (absorbed * (frame.mFogColour + point.xyz)
        + fogDirectionalSegment(ray.mSources, visibility, extinction, ray.mScale, from, to, depth, ray.mClimb));
    air.w *= exp(-depth);
}

// Coverage and lighting live in the grid; height and the water boundary stay on the pixel ray.
vec4 fogVolumeAlong(uvec2 pixel, vec3 origin, vec3 direction, float distance)
{
    vec4 air = vec4(0.0, 0.0, 0.0, 1.0);
    if (!(frame.mFogExtinction > 0.0))
        return air;

    float reach = min(distance, FOG_REACH);
    if (fogPools() && direction.z < 0.0)
        reach = min(reach, max((frame.mWaterLevel - origin.z) / direction.z, 0.0));

    const FogRay ray = FogRay(fogVolumeUV(vec2(pixel) + 0.5, textureSize(fogSlice, 0).xy), origin.z - fogBase(), direction.z,
        FOG_HEIGHT * frame.mFogLift, fogSourcesAlong(direction));
    // Trilinear interpolation changes slope at texel centres. Splitting there preserves narrow lamp peaks.
    float behind = 0.0;
    for (uint slice = 0u; slice <= FOG_VOLUME_SLICES; ++slice)
    {
        if (!(reach > behind))
            break;

        const float ahead = min(froxelMiddle(slice), reach);
        fogSegment(air, ray, behind, ahead);
        behind = ahead;
    }
    return air;
}

/// Integrates unoccluded lamp radiance over a ray segment and samples a lamp for its shadow ray.
// Each light-grid cell owns a disjoint stretch, so a lamp binned in several cells is never counted twice.
// The reservoir weighs each lamp by the same integral: multiplying the sum by the chosen visibility
// gives the expectation of the shadowed sum. Density and view attenuation are applied by the pixel.
vec3 lampsInAir(inout Reservoir kept, inout uint state, vec3 origin, vec3 direction, float entry, float exit)
{
    const float side = 1.0 / frame.mLightGrid.mInverseCell;
    const vec3 beyond = frame.mLightGrid.mOrigin + vec3(frame.mLightGrid.mSize) * side;

    vec3 scattered = vec3(0.0);

    // Clip before the bounded grid walk so empty exterior cells cannot consume its budget.
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

    // A DDA carries cell boundaries without nudging positions across floating-point edges.
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

            const float chord = held.mReach * held.mReach - perpendicular * perpendicular;
            if (!(chord > 0.0))
                continue;

            const float halfChord = sqrt(chord);
            const float from = max(behind, closest - halfChord);
            const float to = min(ahead, closest + halfChord);
            if (!(to > from))
                continue;

            // The froxel holds mean irradiance; correlation with density and view attenuation
            // within this stretch is not resolved by the lighting grid.
            const float crossed
                = falloffAlong(perpendicular, from - closest, to - closest, held.mReach, held.mSourceRadius);

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

/// Closes the culled terrain edge onto the sky gradient.
// Weather alone leaves distant geometry visible. Range follows the terrain culling distance,
// including downward views from mountains; the sky gradient avoids a seam against escaped rays.
vec4 fogEdgeAlong(vec3 direction, float distance)
{
    if (!(frame.mFogEdge > 0.0))
        return vec4(0.0, 0.0, 0.0, 1.0);

    // Upward rays see sky; downward rays must still hide the terrain cut.
    const float rise = 1.0 - smoothstep(0.0, FOG_EDGE_RISE, max(direction.z, 0.0));
    if (!(rise > 0.0))
        return vec4(0.0, 0.0, 0.0, 1.0);

    // Escaped rays carry the camera far distance rather than the terrain reach.
    const float range = min(distance, frame.mFogEdge) / frame.mFogEdge;

    // Normalize the integral of exp(range / FOG_EDGE_RAMP) at the terrain reach.
    const float crossed = (exp(range / FOG_EDGE_RAMP) - 1.0) / (exp(1.0 / FOG_EDGE_RAMP) - 1.0);

    const float transmittance = pow(FOG_EDGE_TRANSMITTANCE, rise * crossed);
    const vec3 haze = skyGradient(frame.mSkyHorizon, frame.mSkyZenith, direction);

    return vec4(haze * (1.0 - transmittance), transmittance);
}

/// Returns scattering in xyz and transmittance in w along the view ray.
// Keep scattering separate so direct light and albedo-demodulated indirect light compose correctly.
// The world-edge haze is behind the weather and receives its full attenuation.
vec4 fogAlong(uvec2 pixel, vec3 origin, vec3 direction, float distance)
{
    // A submerged eye is shaded by the water medium up to its first surface.
    if (waterOver(origin) > 0.0)
        return vec4(0.0, 0.0, 0.0, 1.0);

    const vec4 weather = fogVolumeAlong(pixel, origin, direction, distance);
    const vec4 edge = fogEdgeAlong(direction, distance);
    return vec4(weather.xyz + weather.w * edge.xyz, weather.w * edge.w);
}

#endif
