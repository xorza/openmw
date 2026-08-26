// `#pragma once` everywhere else in this tree, and an include guard here for the reason
// `components/rtx/shaders/portable.h` gives.
#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_WATER_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_WATER_GLSL

// Shading a water surface: Fresnel across a reflection and a refraction, what the column
// under it takes, and the raft of foam where it breaks.

#include "scene.h"
#include "bindings.glsl"
#include "camera.h"
#include "sea.glsl"
#include "shading.glsl"
#include "sky.glsl"
#include "traversal.glsl"
#include "underwater.glsl"

/// How far off the water a reflection or refraction starts. The same bias, for the same reason.
const float WATER_BIAS = SHADOW_BIAS;

/// How far a ray that found no bottom is taken to have travelled through water.
///
/// Twenty-eight metres, past which the transmittance of even blue is under a part in a thousand, so
/// whatever is behind it cannot matter.
const float WATER_MAX_PATH = 2000.0;

/// How squarely a wave facet has to face the ray that found it before it is tilted back toward the
/// plane. Small: a guard against a facet turning away entirely, not a limit on the waves.
const float WATER_MIN_FACING = 0.03;

/// The waterline, over which water with nothing under it becomes the shore beside it.
///
/// Where the ground rises to meet the surface the depth between them goes to zero, and a pixel of
/// water with no water in it has to come out as the ground — otherwise the plane cuts the terrain
/// along a hard line, which is the classic tell of a water plane and is on screen in 533 of the
/// game's 1,292 land cells. Half a metre is enough to hide the intersection without making the
/// shallows look thin.
const float WATER_SHORE_FADE = 35.0;

/// What a ray sent out from the water surface found.
struct WaterPath
{
    /// The light coming back along it, already shaded.
    vec3 mRadiance;

    /// How far it went to find that, or `WATER_MAX_PATH` where it found nothing.
    float mDistance;

    /// Where it landed, in world units, and which instance row that surface came off. Both nought
    /// where nothing was found.
    vec3 mPosition;
    uint mInstance;

    /// The plane of what it landed on, unturned.
    ///
    /// **Carried because the bed's gradient is what places a surf zone**, and the ray that measures
    /// the depth has already resolved it — asking separately would traverse the same ray twice.
    /// Level where nothing was found, which is the one answer that cannot invent a shore: a level
    /// bed has no downhill, and `bedFall` reads that as ground a wave never came across.
    vec3 mGeometric;
};

/// What a ray sees along `direction` after leaving the water, and what it found to see it on.
///
/// **The pixel's own cone, not a bounce's.** A reflection and a refraction are specular: they carry
/// the footprint the primary ray had, where a diffuse bounce spreads over a hemisphere and wants the
/// coarse mip that goes with it. Traced at the bounce rate, a seabed a hundred units down gets a
/// hundred-unit footprint — every texture at its top mip, and every wave averaged out of the
/// caustics the same footprint governs.
///
/// The cone widens by the lobe as well as by the pixel: a reflection off water too fine to resolve
/// is blurred by the slopes that were averaged away, and what it reflects should blur with it.
///
/// **Solid geometry only.** Culling water from its own reflection removes the self-intersection the
/// ray offset exists to avoid, which is what once put a ribbon of flat colour along every waterline:
/// a refraction ray offset to the far side of the plane began under the ground wherever the bed sat
/// nearer the surface than the offset, and reported water of unbounded depth.
///
/// @param seed which draw sequence the lamp reservoir at the far end of this ray steps. The
///        reflection and the refraction take different ones, or both keep the same lamp.
/// @param lobe the rms angle those slopes deflect this ray by — a *radius*, which is why the cone
///        it traces is widened by twice it. Everything `spread` feeds is a width: `resolved` compares
///        it against a wavelength and `coneLod` against a texel area, and `mSpreadAngle` is the whole
///        angle a pixel covers rather than half of one. The sky's disc takes the same number
///        unhalved, because a disc is named by its radius.
WaterPath waterRay(vec3 origin, vec3 direction, float footprint, float lobe, uint seed)
{
    const Surface hit
        = trace(origin, direction, WATER_BIAS, footprint, frame.mCamera.mSpreadAngle + 2.0 * lobe, MASK_SOLID);

    WaterPath path;
    path.mPosition = hit.mPosition;
    path.mInstance = hit.mInstance;
    path.mDistance = hit.mHit ? hit.mDistance : WATER_MAX_PATH;
    path.mGeometric = hit.mHit ? hit.mGeometric : vec3(0.0, 0.0, 1.0);
    path.mRadiance
        = hit.mHit ? shadeSurface(hit, pathEnd(hit.mPosition), seed)
                   : skyRadiance(direction, pixelBlur(frame.mCamera) + lobe);

    return path;
}

/// How fast the bed falls away toward deep water, as a tangent, measured over `span`.
///
/// **A tangent plane is not a beach.** The bed's own normal says how the ground tilts at the point
/// the refraction ray landed on and nothing whatever about whether that tilt carries on: at the rim
/// of a puddle it tilts exactly as a shore does, and reading it as "water deep enough to break in
/// lies just down this slope" is what drew a bright line around every hollow behind Seyda Neen.
/// First-order extrapolation of a slope that stops.
///
/// So the fall is a secant over the distance the answer is wanted for rather than a tangent at one
/// end of it — one ray straight down at the far end — and a bed that stops falling stops counting.
///
/// @param at a point on the water surface.
/// @param geometric the plane of the bed under it.
/// @param here how deep the water is at `at`.
float bedFall(vec3 at, vec3 geometric, float here, float span)
{
    // Downhill, for a plane, is the horizontal part of whichever way its normal points up. A level
    // bed has no downhill and goes nowhere, which is the answer for a pan.
    const vec2 downhill = geometric.z < 0.0 ? -geometric.xy : geometric.xy;
    if (dot(downhill, downhill) < 1.0e-12)
        return 0.0;

    const vec3 from = vec3(at.xy + normalize(downhill) * span, at.z + WATER_BIAS);
    const Surface bed = trace(from, vec3(0.0, 0.0, -1.0), WATER_BIAS, 0.0, 0.0, MASK_SOLID);

    // Nothing under the water plane out there. The ray started in the surface and went down, so a
    // miss means the ground at that end stands above the sea: that way is dry land and not the open
    // water a wave would have come from.
    if (!bed.mHit)
        return 0.0;

    return max(bed.mDistance - here, 0.0) / span;
}

/// What a water surface reflects, which is not where the water is.
struct WaterMirror
{
    /// Where the reflected surface stands, in world units.
    vec3 mAt;

    /// The direction the reflection left along, for the case where it found no surface at all.
    vec3 mAlong;

    /// Which instance row it came off, so the frame can ask where that used to be.
    uint mInstance;

    /// False where the reflection reached the sky, which is a reflection with no distance to it and
    /// not a reflection of nothing: `mAlong` is the whole of the answer there.
    bool mFound;
};

/// What the water sends back along the ray that found it.
/// @param key this pixel's draw key, which the three reservoirs below each offset by their own
///        `SEED_LAMPS_` constant — what the water reflects, what is seen through it and the foam
///        are three surfaces shaded from one hit, and three reservoirs seeded alike keep one lamp.
/// @param mirror what this surface reflects, for the motion vector that describes it. Not found
///        where the reflection reached only sky, or where the water is being looked at from
///        underneath — neither is a thing a mirrored reprojection has an answer for.
vec3 shadeWater(Surface surface, vec3 incident, out SurfaceResponse response, out WaterMirror mirror, uint key)
{
    mirror = WaterMirror(vec3(0.0), vec3(0.0), 0u, false);

    // **Which side of the water a ray is on is a question about the plane, not about a wave.** At a
    // glancing angle a facet can tilt far enough to face away from the ray, and reading that as "the
    // camera is underwater" sends the reflection down into the seabed and turns the far water white.
    // Water is the one surface whose sides have absolute names: it is a horizontal plane, so a ray
    // travelling upward into it came from underneath, whatever the quad's winding says.
    const bool fromBelow = incident.z > 0.0;
    const vec3 plane = fromBelow ? vec3(0.0, 0.0, -1.0) : vec3(0.0, 0.0, 1.0);

    // Keyed off world position rather than anything interpolated, so one cell's surface continues
    // into the next without a seam at the boundary.
    const WaterSurface sea = waterSurfaceAt(surface.mPosition.xy, frame.mTime, surface.mFootprint);

    // A normal tilting by an angle turns its reflection by twice that, so the lobe the lost slopes
    // leave behind is twice their root mean square.
    const float lobe = clamp(2.0 * sqrt(sea.mLostSlope), 0.0, 1.0);
    vec3 normal = fromBelow ? -sea.mNormal : sea.mNormal;

    // A facet still facing away is one the surface would have hidden behind the wave in front of it.
    // Tilting it back toward the plane until it faces the ray is the cheap stand-in for the
    // self-occlusion that is missing, and it is what keeps a glancing reflection finite.
    const float facing = dot(-incident, normal);
    const float flatFacing = dot(-incident, plane);
    if (facing < WATER_MIN_FACING)
    {
        // The dot is linear in the blend, so this is the exact fraction that brings it back to
        // `WATER_MIN_FACING` — solved rather than iterated.
        const float back = (WATER_MIN_FACING - facing) / max(flatFacing - facing, 1e-4);
        normal = normalize(mix(normal, plane, clamp(back, 0.0, 1.0)));
    }

    const float cosine = clamp(dot(-incident, normal), 0.0, 1.0);
    const float fresnel = WATER_F0 + (1.0 - WATER_F0) * pow(1.0 - cosine, 5.0);

    // **The wave's normal and not the quad's**, the lobe the lost slopes left as the roughness, and
    // the Fresnel term as the specular albedo — which is what a specular albedo is, and not what a
    // guide written for a different shading model calls one. The channel is a demodulator: whatever
    // the specular light was multiplied by has to be exactly what is divided back out, and here that
    // is the Fresnel share. `EnvBRDFApprox2` is the answer where the specular half is a pre-integrated
    // GGX lobe; ours is a traced reflection weighted by Schlick, and dividing it by an environment
    // BRDF would divide by a number nothing ever multiplied.
    //
    // Set here so the struct is whole, and settled at each exit below: total internal reflection
    // makes it all of the pixel, and the shore fade and the foam scale it down with the rest of the
    // surface. What is written here reaches no return of its own.
    //
    // Water is the only surface in this renderer with a specular half at all. Every solid reports
    // nought and that is the content's answer rather than a gap: `nifloader.cpp` forces specular to
    // black and glossiness to zero for every mesh at Morrowind's NIF version, because the game had
    // specular lighting disabled — measured across four cells, 831 materials, none with either.
    response = SurfaceResponse(normal, vec3(0.0), vec3(fresnel), lobe);

    // Offset along the *plane*, not the facet: what a ray has to clear to avoid finding this surface
    // again is the quad, and only the plane's normal is guaranteed to take it off that.
    //
    // **Absorption follows whichever ray went into the water, and that flips with the side.** Seen
    // from above, the refraction dives in and the reflection leaves into air; from below, the
    // reflection stays under and the refraction is the sky through Snell's window, which has
    // travelled no water at all. Attenuating the wrong one turns that window green.
    const vec3 leaving = surface.mPosition + plane * WATER_BIAS;

    const vec3 away = reflect(incident, normal);
    const WaterPath bounced = waterRay(leaving, away, surface.mFootprint, lobe, key + SEED_LAMPS_MIRROR);
    vec3 reflected = bounced.mRadiance;
    if (fromBelow)
        reflected = absorbedByWater(reflected, bounced.mDistance);
    else
        mirror = WaterMirror(bounced.mPosition, away, bounced.mInstance, bounced.mDistance < WATER_MAX_PATH);

    const vec3 through = refract(incident, normal, fromBelow ? WATER_IOR : 1.0 / WATER_IOR);
    if (dot(through, through) < 1e-6)
    {
        // Past the critical angle looking up from underwater, where the surface is a mirror and
        // there is nothing behind it to see.
        //
        // **All of it, and the Fresnel term is not that.** Schlick answers a share of the light a
        // surface reflects *when the rest of it refracts*; past the critical angle nothing refracts,
        // so the pixel is the reflection whatever the angle says. At the critical angle itself
        // Schlick gives 0.024, and reporting that of a pixel that is entirely a reflection tells the
        // upscaler to divide the specular light by forty.
        response.mSpecular = vec3(1.0);
        return reflected;
    }

    // Refraction bends by a third of what reflection does, so what is seen *through* the surface is
    // blurred correspondingly less by the same lost slopes.
    const WaterPath behind
        = waterRay(leaving, through, surface.mFootprint, lobe * WATER_REFRACTION_BEND, key + SEED_LAMPS_THROUGH);
    const vec3 refracted = fromBelow ? behind.mRadiance : absorbedByWater(behind.mRadiance, behind.mDistance);

    // With no water left between the surface and the ground, this is the ground. Only from above:
    // seen from under it, the path is a distance through air and says nothing about a shore.
    const float shore = fromBelow ? 1.0 : smoothstep(0.0, WATER_SHORE_FADE, behind.mDistance);
    const vec3 water = mix(behind.mRadiance, mix(refracted, reflected, fresnel), shore);

    // The fade scales the reflection with the rest of the surface, so the share of the pixel that is
    // a reflection falls with it. From below it is one and this changes nothing.
    response.mSpecular = vec3(fresnel * shore);

    if (fromBelow)
        return water;

    // **How deep the water is, and not how far the ray went through it.** Refraction bends toward
    // the vertical, so a grazing view's path to the bed is up to half again the depth under the
    // point — enough to put the surf line in a different place depending on where it is watched
    // from, which is the one thing a shoreline may not do. The drop is the path times its own
    // vertical share, which is exact.
    const float under = behind.mDistance * max(-through.z, 0.0);

    // **Not faded out with the water, which is the one composition that looked obvious and is
    // wrong.** `shore` exists to hide the plane cutting the terrain, and at this sea state it covers
    // fifty centimetres of depth where the whole surf zone is seventeen — so multiplying by it left
    // four per cent foam at its strongest and none at all at the waterline, which is where a
    // shoreline is whitest. Foam is not a tint of the water column, it is a raft floating on top of
    // one; where the column is too thin to tint anything the raft is still there, and the last
    // stretch of it lying over wet sand is what the edge of the sea looks like. It ends where the
    // water does on its own, because past that no ray finds water to stand on.
    const float breaks = foamBreaking(sea, under);
    if (!(breaks > 0.0))
        return water;

    // **The one place a ray is spent on foam, and it is spent only where there could be foam.**
    // Every pixel of open sea reaches the line above and stops there; what gets this far is the
    // shallows, which is a thin band of any frame.
    // How far the probe looks and how far foam lasts are one number, so the model never asks about
    // ground whose answer it would have discounted anyway.
    const float runout = foamRunout(sea);
    const float fall = bedFall(surface.mPosition, behind.mGeometric, under, runout);
    const float covered = breaks * foamReaching(sea, under, fall, runout);
    if (!(covered > 0.0))
        return water;

    // A raft of bubbles is diffuse, and what it covers of the pixel it takes from the reflection.
    response.mSpecular = vec3(fresnel * shore * (1.0 - covered));

    // **Broken water is a raft of bubbles rather than a surface**: white, diffuse, and hiding what is
    // under it rather than tinting it. Lit the way every other diffuse surface in the frame is lit,
    // which is what keeps a beach and the surf running along it in the same sun.
    const vec3 foam
        = WATER_FOAM_ALBEDO
        * (gather(surface.mPosition, sea.mNormal, surface.mFootprint, key + SEED_LAMPS_FOAM)
            + pathEnd(surface.mPosition));

    return mix(water, foam, covered);
}

#endif
