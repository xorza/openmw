#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_TANGENT_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_TANGENT_GLSL

// A vertex's tangent word, `TANGENT_*` in `scene.h`, read and written on the device. `Rtx::packTangent`
// and `Rtx::unpackTangent` are the host's, and the skin pass's test holds the two to each other.

#include "scene.h"

#include "basis.glsl"

/// `value` stepped to `TANGENT_STEPS` either side of nought, rounded half away from nought as the
/// host's `std::lround` is, so a coordinate on a half step lands where the host puts it.
uint tangentStep(float value)
{
    const float scaled = clamp(value, -1.0, 1.0) * float(TANGENT_STEPS);
    return uint(int(sign(scaled) * floor(abs(scaled) + 0.5)) + int(TANGENT_STEPS));
}

float tangentCoordinate(uint stored)
{
    return (float(stored) - float(TANGENT_STEPS)) / float(TANGENT_STEPS);
}

/// The word for `direction`, with the bitangent's handedness `flipped`, or nought — no tangent —
/// for a direction of no length.
uint packTangent(vec3 direction, bool flipped)
{
    if (!(abs(direction.x) + abs(direction.y) + abs(direction.z) > 0.0))
        return 0u;

    const vec2 square = octahedralSquare(direction);
    return TANGENT_PRESENT | (flipped ? TANGENT_FLIPPED : 0u) | tangentStep(square.x)
        | (tangentStep(square.y) << TANGENT_COORDINATE_BITS);
}

/// The unit direction and the handedness `packed` holds, or nought where it holds none.
vec4 unpackTangent(uint packed)
{
    if ((packed & TANGENT_PRESENT) == 0u)
        return vec4(0.0);

    const vec2 square = vec2(tangentCoordinate(packed & TANGENT_COORDINATE_MASK),
        tangentCoordinate((packed >> TANGENT_COORDINATE_BITS) & TANGENT_COORDINATE_MASK));
    return vec4(octahedralUnit(square), (packed & TANGENT_FLIPPED) != 0u ? -1.0 : 1.0);
}

#endif
