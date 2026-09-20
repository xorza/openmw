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

/// A unit vector as a point of the square, and back: Cigolle et al.'s octahedral map, which folds
/// the lower hemisphere out over the upper one's corners so that the whole sphere is one square
/// with no seam a filter would notice and no pole where precision runs out.
///
/// **In one word of two signed halves**, which is the payload's use of it. Sixteen bits an axis is
/// a direction to a hundredth of a degree — finer than the half-float channels the guide is
/// stored in downstream, so nothing the frame keeps is lost across the execute.
///
/// @param unit a unit vector; a nought vector has no direction and is the caller's to keep apart.
uint packDirection(vec3 unit)
{
    const vec3 folded = unit / (abs(unit.x) + abs(unit.y) + abs(unit.z));
    const vec2 upper = folded.xy;
    const vec2 lower = (1.0 - abs(folded.yx)) * vec2(folded.x >= 0.0 ? 1.0 : -1.0, folded.y >= 0.0 ? 1.0 : -1.0);

    return packSnorm2x16(folded.z >= 0.0 ? upper : lower);
}

vec3 unpackDirection(uint packed)
{
    const vec2 square = unpackSnorm2x16(packed);
    vec3 unit = vec3(square, 1.0 - abs(square.x) - abs(square.y));
    if (unit.z < 0.0)
        unit.xy = (1.0 - abs(unit.yx)) * vec2(unit.x >= 0.0 ? 1.0 : -1.0, unit.y >= 0.0 ? 1.0 : -1.0);

    return normalize(unit);
}

#endif
