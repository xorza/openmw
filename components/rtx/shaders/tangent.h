#ifndef OPENMW_COMPONENTS_RTX_SHADERS_TANGENT_H
#define OPENMW_COMPONENTS_RTX_SHADERS_TANGENT_H

#include "hosttypes.h"
#include "portable.h"
#include "scene.h"

// A direction folded onto a square and back, and a vertex's tangent word built on it: written once
// for both sides, because the host packs what the device unpacks and the skin pass packs again, and
// a word two hands spelled is two words wherever they round apart.
//
// Scalar arithmetic over a vector's components, `v[i]`, which is the one spelling both languages
// read: GLSL's swizzles and OpenSceneGraph's accessors have none in common.

#ifdef RTX_HOST
namespace Rtx::Shaders
{
#endif

    /// A direction as a point of the square: Cigolle et al.'s octahedral map, which folds the lower
    /// hemisphere out over the upper one's corners so that the whole sphere is one square with no
    /// seam a filter would notice and no pole where precision runs out.
    ///
    /// @param direction of any length but nought, which has no direction and is the caller's to keep
    ///        apart.
    RTX_SHADER vec2 octahedralSquare(vec3 direction)
    {
        const float sum = abs(direction[0]) + abs(direction[1]) + abs(direction[2]);
        const float x = direction[0] / sum;
        const float y = direction[1] / sum;
        const bool upper = direction[2] >= 0.0f;

        // Selected and not branched, so a lane takes one path whichever half it is in.
        const float lowerX = (1.0f - abs(y)) * (x >= 0.0f ? 1.0f : -1.0f);
        const float lowerY = (1.0f - abs(x)) * (y >= 0.0f ? 1.0f : -1.0f);
        return vec2(upper ? x : lowerX, upper ? y : lowerY);
    }

    /// The point of the square back to the unit direction it stands for.
    RTX_SHADER vec3 octahedralUnit(vec2 square)
    {
        const float x = square[0];
        const float y = square[1];
        const float z = 1.0f - abs(x) - abs(y);
        const bool upper = z >= 0.0f;

        const float lowerX = (1.0f - abs(y)) * (x >= 0.0f ? 1.0f : -1.0f);
        const float lowerY = (1.0f - abs(x)) * (y >= 0.0f ? 1.0f : -1.0f);
        return normalize(vec3(upper ? x : lowerX, upper ? y : lowerY, z));
    }

    /// `value` stepped to `TANGENT_STEPS` either side of nought, rounded half away from nought.
    RTX_SHADER uint tangentStep(float value)
    {
        const float scaled = clamp(value, -1.0f, 1.0f) * float(TANGENT_STEPS);
        const float rounded = scaled >= 0.0f ? floor(scaled + 0.5f) : -floor(0.5f - scaled);
        return uint(int(rounded) + int(TANGENT_STEPS));
    }

    RTX_SHADER float tangentCoordinate(uint stored)
    {
        return (float(stored) - float(TANGENT_STEPS)) / float(TANGENT_STEPS);
    }

    /// The word for `direction`, with the bitangent's handedness `flipped`, or nought — no tangent —
    /// for a direction of no length, or of none a number can hold.
    RTX_SHADER uint packTangent(vec3 direction, bool flipped)
    {
        if (!(abs(direction[0]) + abs(direction[1]) + abs(direction[2]) > 0.0f))
            return 0u;

        const vec2 square = octahedralSquare(direction);
        return TANGENT_PRESENT | (flipped ? TANGENT_FLIPPED : 0u) | tangentStep(square[0])
            | (tangentStep(square[1]) << TANGENT_COORDINATE_BITS);
    }

    /// The unit direction and the handedness `packed` holds, or nought where it holds none.
    RTX_SHADER vec4 unpackTangent(uint packed)
    {
        if ((packed & TANGENT_PRESENT) == 0u)
            return vec4(0.0f, 0.0f, 0.0f, 0.0f);

        const vec2 square = vec2(tangentCoordinate(packed & TANGENT_COORDINATE_MASK),
            tangentCoordinate((packed >> TANGENT_COORDINATE_BITS) & TANGENT_COORDINATE_MASK));
        return vec4(octahedralUnit(square), (packed & TANGENT_FLIPPED) != 0u ? -1.0f : 1.0f);
    }

#ifdef RTX_HOST
}
#endif

#endif
