#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_BASIS_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_BASIS_GLSL

// A frame built on one axis, for anything that draws about a direction or lays a grid across one.
//
// **Its own file because a pass with no frame block wants it.** `random.glsl` builds every sampled
// direction on it and reaches the frame for its draws; `spriteshade.comp` lays its grid on it and
// has no frame at all, and wrote its own copy with a different threshold.

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

#endif
