// `#pragma once` everywhere else in this tree, and an include guard here for the reason
// `components/rtx/shaders/portable.h` gives.
#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_FROXEL_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_FROXEL_GLSL

#include "look.h"
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
/// Fixed boundaries keep temporal samples and pixel integration on the same depth distribution.
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

#endif
