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
///
/// **One block for the three indices**, because a mesh's index run never straddles one — the
/// argument `normalBlockOf` makes for the corners it hands back.
uvec3 triangleCorners(GpuMesh mesh, uint primitive)
{
    const uint triangle = mesh.mIndexOffset + primitive * 3u;
    IndexBlock block = indexBlockOf(triangle);
    const uint at = triangle % INDEX_BLOCK;

    return mesh.mVertexOffset + uvec3(block.at[at], block.at[at + 1u], block.at[at + 2u]);
}

/// What each corner contributes at a hit, from the two barycentrics a query reports.
vec3 cornerWeights(vec2 bary)
{
    return vec3(1.0 - bary.x - bary.y, bary.x, bary.y);
}

/// The texture coordinates of the triangle a hit landed on.
void triangleUvs(uvec3 corner, out vec2 uv[3])
{
    TexCoordBlock block = texCoordBlockOf(corner.x);
    const uvec3 at = corner % VERTEX_BLOCK;

    uv[0] = block.at[at.x];
    uv[1] = block.at[at.y];
    uv[2] = block.at[at.z];
}

/// The vertex normal interpolated across the triangle a hit landed on, in the mesh's own space and
/// not yet unit: a mesh with no normals holds zeros, which the caller reads as "use the plane".
vec3 triangleNormal(uvec3 corner, vec3 weight)
{
    NormalBlock block = normalBlockOf(corner.x);
    const uvec3 at = corner % VERTEX_BLOCK;

    return block.at[at.x] * weight.x + block.at[at.y] * weight.y + block.at[at.z] * weight.z;
}

vec2 interpolate(vec2 uv[3], vec3 weight)
{
    return uv[0] * weight.x + uv[1] * weight.y + uv[2] * weight.z;
}

#endif
