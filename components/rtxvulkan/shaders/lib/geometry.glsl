// `#pragma once` everywhere else in this tree, and an include guard here for the reason
// `components/rtx/shaders/portable.h` gives.
#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_GEOMETRY_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_GEOMETRY_GLSL

// What a hit is, before any material is read: which vertices, what each weighs, and the
// plane the triangle lies in.

#include "scene.h"
#include "bindings.glsl"

/// Twice the area of a hit triangle, as a vector along its plane's normal.
///
/// Object to world is a rotation, a uniform scale and a translation, so a direction survives it —
/// and the translation cancels in an edge, so the upper 3x3 is all an edge needs. One cross product
/// then answers two questions: normalised it is the plane's normal, and its length is the size a
/// cone has to compare its own against.
vec3 triangleCross(vec3 corners[3], mat4x3 toWorld)
{
    return cross(mat3(toWorld) * (corners[1] - corners[0]), mat3(toWorld) * (corners[2] - corners[0]));
}

/// Where in the shared vertex buffers the three corners of a mesh's triangle are.
uvec3 triangleCorners(GpuMesh mesh, uint primitive)
{
    const uint triangle = mesh.mIndexOffset + primitive * 3u;
    return mesh.mVertexOffset + uvec3(indexAt(triangle), indexAt(triangle + 1u), indexAt(triangle + 2u));
}

/// What each corner contributes at a hit, from the two barycentrics a query reports.
vec3 cornerWeights(vec2 bary)
{
    return vec3(1.0 - bary.x - bary.y, bary.x, bary.y);
}

/// The texture coordinates of the triangle a hit landed on.
void triangleUvs(uvec3 corner, out vec2 uv[3])
{
    uv[0] = texCoordAt(corner.x);
    uv[1] = texCoordAt(corner.y);
    uv[2] = texCoordAt(corner.z);
}

vec2 interpolate(vec2 uv[3], vec3 weight)
{
    return uv[0] * weight.x + uv[1] * weight.y + uv[2] * weight.z;
}

#endif
