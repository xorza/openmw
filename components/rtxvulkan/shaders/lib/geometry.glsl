#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_GEOMETRY_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_GEOMETRY_GLSL

// What a hit is, before any material is read: which vertices, what each weighs, and the
// plane the triangle lies in.

#include "scene.h"
#include "bindings.glsl"
#include "tangent.glsl"

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

/// The same off the mesh's second set, which sits in blocks of its own at the mesh's own offset —
/// `GpuMesh::mSecondTexCoordOffset`. The caller has asked whether there is one.
void triangleSecondUvs(GpuMesh mesh, uvec3 corner, out vec2 uv[3])
{
    const uvec3 second = corner - mesh.mVertexOffset + mesh.mSecondTexCoordOffset;
    TexCoordBlock block = secondTexCoordBlockOf(second.x);
    const uvec3 at = second % VERTEX_BLOCK;

    uv[0] = block.at[at.x];
    uv[1] = block.at[at.y];
    uv[2] = block.at[at.z];
}

/// Whether `unit` reads the mesh's second set — `GpuMesh::mUnitStreams` — and there is one.
bool readsSecondUvs(GpuMesh mesh, uint unit)
{
    return mesh.mSecondTexCoordOffset != NO_STREAM && ((mesh.mUnitStreams >> unit) & 1u) != 0u;
}

/// The vertex normals of the triangle a hit landed on, in the mesh's own space and not yet unit:
/// a mesh with no normals holds zeros, which the caller reads as "use the plane".
void triangleNormals(uvec3 corner, out vec3 normal[3])
{
    NormalBlock block = normalBlockOf(corner.x);
    const uvec3 at = corner % VERTEX_BLOCK;

    normal[0] = block.at[at.x];
    normal[1] = block.at[at.y];
    normal[2] = block.at[at.z];
}

/// The same interpolated across the triangle, which is what a hit shades with.
vec3 triangleNormal(uvec3 corner, vec3 weight)
{
    vec3 normal[3];
    triangleNormals(corner, normal);

    return normal[0] * weight.x + normal[1] * weight.y + normal[2] * weight.z;
}

/// The vertex tangents interpolated across the triangle, in the mesh's own space, with the
/// bitangent's handedness in `w` interpolated with them — which is what the rasterizer's
/// `passTangent` is, and what `normals.glsl` builds its frame from. Nought where the mesh carries
/// none, and the caller has asked whether it carries any — `MESH_TANGENTS`.
vec4 triangleTangent(uvec3 corner, vec3 weight)
{
    TangentBlock block = tangentBlockOf(corner.x);
    const uvec3 at = corner % VERTEX_BLOCK;

    return unpackTangent(block.at[at.x]) * weight.x + unpackTangent(block.at[at.y]) * weight.y
        + unpackTangent(block.at[at.z]) * weight.z;
}

/// How far the point a hit landed on moved since the previous frame, in the mesh's own space:
/// this frame's pose less the previous frame's at the same barycentric point of the same
/// triangle. Nought exactly for a mesh that did not move, whose two copies hold the same numbers.
/// The caller has asked whether the mesh deforms — `GpuMesh::mBindOffset`.
///
/// **In object space, because a difference of two positions is exact there.** Both poses are
/// a body's own coordinates, a few hundred units at most, and a limb's step between frames is
/// a few of them; the world's six-figure coordinates would spend the step in rounding, which is
/// the argument `movedBy` makes for the rigid half.
///
/// @param corner the global vertex ids `triangleCorners` hands back, which are the mesh's
///        vertex offset plus the index within the mesh; the pose blocks are addressed by the
///        bind offset plus that index.
vec3 triangleDeformation(GpuMesh mesh, uvec3 corner, vec3 weight)
{
    const uvec3 posed = corner - mesh.mVertexOffset + mesh.mBindOffset;
    const uvec3 at = posed % VERTEX_BLOCK;
    NormalBlock now = poseBlockOf(posed.x);
    NormalBlock was = previousPoseBlockOf(posed.x);

    return (now.at[at.x] - was.at[at.x]) * weight.x + (now.at[at.y] - was.at[at.y]) * weight.y
        + (now.at[at.z] - was.at[at.z]) * weight.z;
}

/// The vertex colour interpolated across the triangle a hit landed on, in linear light.
///
/// **White where the mesh brought none, because that is what the table holds.** A colour is a
/// factor and not a reading: `MeshTable::writeVertices` fills an absent one with ones, so this
/// answers neutrally and nothing past it has a case to test.
///
/// **Interpolated in light, and not between two stored bytes.** The host decodes each vertex once
/// — `Rtx::MeshArrays::mColours` — so what a hit reads across a triangle is a blend of
/// reflectances rather than a blend of the numbers they were written down as.
vec3 triangleColour(uvec3 corner, vec3 weight)
{
    ColourBlock block = colourBlockOf(corner.x);
    const uvec3 at = corner % VERTEX_BLOCK;

    return block.at[at.x] * weight.x + block.at[at.y] * weight.y + block.at[at.z] * weight.z;
}

vec2 interpolate(vec2 uv[3], vec3 weight)
{
    return uv[0] * weight.x + uv[1] * weight.y + uv[2] * weight.z;
}

#endif
