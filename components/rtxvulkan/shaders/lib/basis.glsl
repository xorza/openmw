#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_BASIS_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_BASIS_GLSL

// Directions, with nothing of the frame block in them: a frame built on one axis, for anything that
// draws about a direction or lays a grid across one; a normal turned to face the ray that found it;
// and a direction in one word, on the octahedral map `tangent.h` shares with the host.
//
// **Its own file because a pass with no frame block wants it.** `random.glsl` builds every sampled
// direction on it and reaches the frame for its draws; `spriteshade.comp` lays its grid on it and
// has no frame at all, and wrote its own copy with a different threshold.

#include "tangent.h"

/// A unit vector square to `axis`, to build a basis on.
///
/// Any vector not parallel to it will do, and which one is arbitrary — so the only thing this owes
/// a caller is that the cross product it takes never collapses: the helper is whichever world axis
/// `axis` lies least along.
vec3 tangentTo(vec3 axis)
{
    const vec3 aside = abs(axis.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    return normalize(cross(aside, axis));
}

/// A shading normal turned back toward `fallback` until it faces the ray that found it at `least`,
/// where it faced that ray less: Schüssler et al.'s problem, answered the cheap way. A normal a wave
/// or a normal map leans past the ray has no light to give back along it, and the self-occlusion
/// that would have hidden it is what a tilt stands in for. One statement for the water and for a
/// mapped solid, so the two cannot come to answer it differently.
///
/// @param fallback the normal that faces the ray, or faces it more — the water's plane, a solid's
///        interpolated normal. Where it does not face it either, the blend stops at it.
vec3 facingRay(vec3 normal, vec3 fallback, vec3 incident, float least)
{
    const float facing = dot(-incident, normal);
    if (!(facing < least))
        return normal;

    // The dot is linear in the blend, so this is the exact fraction that brings it back to `least` —
    // solved rather than iterated.
    const float back = (least - facing) / max(dot(-incident, fallback) - facing, 1e-4);
    return normalize(mix(normal, fallback, clamp(back, 0.0, 1.0)));
}

/// The octahedral map in one word of two signed halves, which is the payload's use of it. Sixteen
/// bits an axis is a direction to a hundredth of a degree — finer than the half-float channels the
/// guide is stored in downstream, so nothing the frame keeps is lost across the execute.
uint packDirection(vec3 unit)
{
    return packSnorm2x16(octahedralSquare(unit));
}

vec3 unpackDirection(uint packed)
{
    return octahedralUnit(unpackSnorm2x16(packed));
}

#endif
