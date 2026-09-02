// `#pragma once` everywhere else in this tree, and an include guard here for the reason
// `components/rtx/shaders/portable.h` gives.
#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_FROXEL_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_FROXEL_GLSL

// Where the fog volume's slices stand, which four shaders have to agree about exactly.
//
// **Its own file because three of the four want nothing else from the air.** `fogscatter.comp`
// fills a froxel, `fogintegrate.comp` carries the transmittance down a column and `fogVolumeAlong`
// resolves a distance to a slice — and a shader that only asks where a slice starts should not have
// to pull in a phase function, a light grid and a ray query to find out.
//
// One statement of the curve, so a boundary the scatter pass sampled inside is the boundary the
// integrate pass takes its transmittance over.

#include "scene.h"

/// Where along the ray the slice ending at `fraction` of the way through reaches.
///
/// **Squared, so the slices bunch where the fog has any shape to it.** Even steps over a ray that
/// can run thirty thousand units give the first hundred a twentieth of one sample and lay the rest
/// across ground too far off to resolve — the same reasoning that makes every froxel grid slice its
/// frustum exponentially rather than evenly.
float fogDepth(float fraction)
{
    return fraction * fraction;
}

/// How far in front of the eye `slice` begins and ends, in world units.
///
/// **The whole reach and not the distance to a surface**, which is the one thing a volume cannot
/// know: it is filled before anything has been traced.
float froxelNear(uint slice)
{
    return fogDepth(float(slice) / float(FOG_VOLUME_SLICES)) * FOG_REACH;
}

float froxelFar(uint slice)
{
    return fogDepth(float(slice + 1u) / float(FOG_VOLUME_SLICES)) * FOG_REACH;
}

/// How thick the grid is where a point `along` the ray stands, in world units: the stride of the
/// slice that holds it, as a smooth function of where the point is rather than of which slice that
/// is.
///
/// **Continuous, because a step in it is a shell around the eye.** The field is read at the level
/// its sampler can resolve over this distance, and read at each slice's own stride that level steps
/// at every boundary between two slices — a bank's edge blurred by one amount on the near side of
/// the shell and by another beyond it. The shell is the eye's, so it sweeps the ground as the eye
/// moves: rings around the camera, faint while it stood still and plain as soon as it walked. The
/// derivative of the depth curve, so a slice's own middle reads exactly its own stride and the
/// sample drawn either side of it reads a little less or a little more.
float froxelStrideAt(float along)
{
    return 2.0 * sqrt(along * FOG_REACH) / float(FOG_VOLUME_SLICES);
}

/// Where the slice stands, as the one point that answers for the whole of it.
///
/// **Half way *through* the slice and not half way *between* its edges.** The curve is quadratic, so
/// those are two different distances — and the first is the one the grid's own depth coordinate
/// carries, because that coordinate is this curve inverted. What reads a volume at a distance is a
/// sampler, and a sampler puts texel `k` at `(k + 0.5)` of its axis; hand it the mean of the edges
/// instead and a slice near the eye asks for a fifth of a texel past its own centre, so a
/// reprojection that should have found the froxel it left finds a fifth of the froxel behind it.
float froxelMiddle(uint slice)
{
    return fogDepth((float(slice) + 0.5) / float(FOG_VOLUME_SLICES)) * FOG_REACH;
}

/// What one slice of a column scatters and takes out, once everything that lights it is applied:
/// the air's own colour with the moons and the lamps in it, the extinction per world unit, and the
/// sun's transport with the irradiance and the phase left off.
///
/// **A sample at the slice's middle, and not a constant over the slice.** The volume holds one of
/// these per froxel, and what a froxel's value is is a property of one point in it, averaged over
/// draws — so between two of them the air is what a sampler says it is between two texels: the
/// line from one to the next. `fogThrough` integrates that line, and `fogintegrate.comp` and
/// `fogVolumeAlong` both step through it, so a column's accumulation and a pixel's read of the
/// slice it ends in agree exactly at the slice's far edge.
///
/// **Constant over a slice instead, the grid drew itself.** The accumulation was then a straight
/// line across every slice with a corner at every edge — or, reconstructed with a cubic to hide the
/// corners, a bump in the middle of every slice — and either is a pattern with the slices' own
/// period, laid on the ground as shells around the eye wherever two neighbouring slices held
/// different air. Banked air holds different air in neighbouring slices everywhere.
struct FogSlice
{
    vec3 mInscatter;
    float mExtinction;
    vec3 mSunward;
};

FogSlice fogSliceBetween(FogSlice from, FogSlice to, float fraction)
{
    return FogSlice(mix(from.mInscatter, to.mInscatter, fraction), mix(from.mExtinction, to.mExtinction, fraction),
        mix(from.mSunward, to.mSunward, fraction));
}

/// Carries a ray `length` units through air of `slice`, accumulating what it scattered in and
/// taking off what it lost.
///
/// What this stretch is worth to the frame, computed once and used twice: what it scatters in is
/// weighted by it, and what the transmittance loses to it is exactly it, since `T * (1 - absorbed)`
/// is `T - T * absorbed`.
///
/// **Exact for a stretch the line does not bend in**, which is why both callers cut a slice at its
/// middle: the line from one slice's sample to the next bends only at the samples, so each half of
/// a slice is one straight piece and its mean is its own middle.
void fogThrough(inout float transmittance, inout vec3 scattered, inout vec3 sunward, FogSlice slice, float length)
{
    const float weight = transmittance * (1.0 - exp(-slice.mExtinction * length));

    scattered += weight * slice.mInscatter;
    sunward += weight * slice.mSunward;
    transmittance -= weight;
}

#endif
