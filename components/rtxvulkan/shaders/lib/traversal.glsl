// `#pragma once` everywhere else in this tree, and an include guard here for the reason
// `components/rtx/shaders/portable.h` gives.
#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_TRAVERSAL_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_TRAVERSAL_GLSL

// Traversal, and what a ray found resolved down to the inputs shading needs.
//
// **No light here.** That is what lets water shade by tracing again: a reflection's hit is
// resolved by this same `trace` and shaded by `shadeSurface`, and neither calls back into
// water — which a shader with no recursion could not survive.

#include "scene.h"
#include "bindings.glsl"
#include "geometry.glsl"
#include "texturing.glsl"

/// Untextured surfaces are mid-grey rather than black, so a missing texture reads as missing rather
/// than as shadow.
const vec3 NO_TEXTURE_ALBEDO = vec3(0.5);

/// How far off a surface a shadow ray starts, in world units.
///
/// A Morrowind unit is about 1.4 cm, and a float at the far side of a worldspace resolves to a
/// hundredth of one — so this is invisible and still an order of magnitude clear of where a hit
/// point can land on the wrong side of its own triangle.
const float SHADOW_BIAS = 1.0;

/// Whether a candidate hit landed on the material or in one of its holes.
///
/// Only instances the build marked non-opaque reach this, and it marked exactly the materials with
/// a mask to read, so there is no mode to branch on here: the comparison is the whole test, and a
/// surface that wants none stores a threshold nothing can fail.
///
/// The level it reads at matters as much as the test does. A mask point-sampled at its finest mip
/// answers for one texel out of the hundreds a distant pixel covers, and a binary test on that is a
/// coin toss per pixel — a canopy comes back as speckle, and it crawls as the camera moves. Letting
/// the cone average the mask first costs a leaf edge some of its bite, which is by a long way the
/// better of the two errors.
bool alphaPasses(uint instanceIndex, uint primitive, vec2 bary, vec3 crossed, vec3 direction, float coneWidth)
{
    const GpuInstance instance = instances[instanceIndex];
    const GpuMaterial material = materials[instance.mMaterial];

    vec2 uv[3];
    triangleUvs(triangleCorners(meshes[instance.mMesh], primitive), uv);

    const vec4 texel = sampleDiffuse(
        material.mDiffuse, uv, cornerWeights(bary), material.mTextureTransform, crossed, direction, coneWidth);

    return texel.a >= material.mAlphaCutoff;
}

/// The candidate loop, run to completion, confirming every hit that lands on the material rather
/// than in one of its holes.
///
/// **A macro because `glslc` rejects `rayQueryEXT` as an `out` or `inout` parameter**, so a
/// traversal cannot be handed to a function and this cannot be one. It was written out twice, and
/// the comment above the second copy said what that costs: any change to the cutout had to be made
/// in both places. The preprocessor is the one construct that survives the restriction.
///
/// @param query a traversal already initialised, which this drives to completion.
/// @param along the direction the ray travels, which the cutout resolves its mip against.
/// @param cone how wide the ray's cone is *at this candidate*, which is what decides how much of the
///        mask one pixel is looking at. Nought for a ray that carries no cone, which reads the
///        finest level — every shadow ray. Substituted textually, so it may name the traversal.
#define RTX_RESOLVE_CUTOUTS(query, along, cone)                                                              \
    while (rayQueryProceedEXT(query))                                                                        \
    {                                                                                                        \
        if (rayQueryGetIntersectionTypeEXT(query, false) != gl_RayQueryCandidateIntersectionTriangleEXT)      \
            continue;                                                                                        \
                                                                                                             \
        vec3 candidateCorners[3];                                                                            \
        rayQueryGetIntersectionTriangleVertexPositionsEXT(query, false, candidateCorners);                    \
                                                                                                             \
        if (alphaPasses(rayQueryGetIntersectionInstanceCustomIndexEXT(query, false),                          \
                rayQueryGetIntersectionPrimitiveIndexEXT(query, false),                                       \
                rayQueryGetIntersectionBarycentricsEXT(query, false),                                         \
                triangleCross(candidateCorners, rayQueryGetIntersectionObjectToWorldEXT(query, false)),       \
                (along), (cone)))                                                                             \
            rayQueryConfirmIntersectionEXT(query);                                                            \
    }

/// Whether anything stands between `from` and a light `reach` away along `towards`.
///
/// No cone here, so the cutout is decided at the finest mip. A shadow ray carries no footprint, and
/// aliasing in a leaf's shadow is worth far less than aliasing on the leaf.
///
/// **A ray shorter than the bias it starts past is not a ray.** A candle sitting a unit off a table
/// asks for a shadow ray whose end is behind its own beginning, and `rayQueryInitializeEXT` with a
/// `tmax` under its `tmin` is undefined — which is a hang or a garbage answer rather than an empty
/// one. Nothing fits in that gap anyway: the bias is what a hit point's own surface needs to be
/// clear of, so a light inside it is a light nothing can stand between.
bool occluded(vec3 from, vec3 towards, float distance)
{
    if (distance <= SHADOW_BIAS)
        return false;

    rayQueryEXT query;
    rayQueryInitializeEXT(
        query, sceneTop, gl_RayFlagsTerminateOnFirstHitEXT, MASK_SOLID, from, SHADOW_BIAS, towards, distance);
    RTX_RESOLVE_CUTOUTS(query, towards, 0.0)

    return rayQueryGetIntersectionTypeEXT(query, true) != gl_RayQueryCommittedIntersectionNoneEXT;
}

/// What a ray found, resolved down to the inputs shading needs.
///
/// Geometry and material only — no light. That is what lets water shade by tracing again: the
/// reflection's hit is resolved by this same function and shaded by `shadeSurface`, and neither
/// calls back into water, which a shader with no recursion could not survive.
struct Surface
{
    bool mHit;
    bool mWater;

    vec3 mPosition;

    /// The shading normal, turned to the side of the triangle's plane the ray arrived on.
    /// Morrowind's sheet geometry is lit from both faces, so which side that is carries no meaning
    /// beyond where the light may come from — and the *plane* is what decides it, never the
    /// interpolated normal, which on this content routinely points through its own triangle.
    vec3 mNormal;

    /// The triangle's own plane, unturned — which is what a question about *sides* has to ask.
    vec3 mGeometric;

    vec3 mAlbedo;

    /// The material's own glow, as a lighting term. See `GpuMaterial::mEmissiveColour`.
    vec3 mEmissiveColour;

    /// What its emissive map adds past the albedo, already scaled.
    vec3 mEmitted;

    float mDistance;

    /// Which row of the instance table this came off, so the frame can ask where it used to be.
    uint mInstance;

    /// How wide the ray's cone was where it landed. Everything sampled here was averaged over it,
    /// and so is everything the light arriving here was.
    float mFootprint;
};

/// Traverses, and resolves whatever it hit.
///
/// @param footprint how wide the ray's cone starts, which for a primary ray is nothing and for a
///        reflection is whatever the pixel had already spread to at the water.
/// @param spread how much wider that cone gets per unit travelled.
Surface trace(vec3 origin, vec3 direction, float tmin, float footprint, float spread, uint mask)
{
    Surface surface;
    surface.mHit = false;
    surface.mWater = false;
    surface.mPosition = origin;
    surface.mNormal = vec3(0.0, 0.0, 1.0);
    surface.mGeometric = vec3(0.0, 0.0, 1.0);
    surface.mAlbedo = vec3(0.0);
    surface.mEmissiveColour = vec3(0.0);
    surface.mEmitted = vec3(0.0);
    surface.mDistance = frame.mFar;
    surface.mFootprint = 0.0;

    rayQueryEXT query;
    // No blanket opaque flag: the per-instance bits the build set from each material are what decide
    // whether traversal stops to ask, and forcing opacity here would override them and put every
    // leaf back inside the card it was painted on.
    rayQueryInitializeEXT(query, sceneTop, gl_RayFlagsNoneEXT, mask, origin, tmin, direction, frame.mFar);
    RTX_RESOLVE_CUTOUTS(query, direction, footprint + spread * rayQueryGetIntersectionTEXT(query, false))

    if (rayQueryGetIntersectionTypeEXT(query, true) == gl_RayQueryCommittedIntersectionNoneEXT)
        return surface;

    surface.mHit = true;
    surface.mDistance = rayQueryGetIntersectionTEXT(query, true);
    surface.mPosition = origin + direction * surface.mDistance;

    surface.mFootprint = footprint + spread * surface.mDistance;

    surface.mInstance = rayQueryGetIntersectionInstanceCustomIndexEXT(query, true);

    const GpuInstance instance = instances[surface.mInstance];
    const uvec3 corner
        = triangleCorners(meshes[instance.mMesh], rayQueryGetIntersectionPrimitiveIndexEXT(query, true));
    const vec3 weight = cornerWeights(rayQueryGetIntersectionBarycentricsEXT(query, true));

    const mat4x3 toWorld = rayQueryGetIntersectionObjectToWorldEXT(query, true);

    // The plane's normal is always available; the vertices' is not, and is better where it is.
    vec3 corners[3];
    rayQueryGetIntersectionTriangleVertexPositionsEXT(query, true, corners);
    const vec3 crossed = triangleCross(corners, toWorld);
    surface.mGeometric = dot(crossed, crossed) > 0.0 ? normalize(crossed) : vec3(0.0, 0.0, 1.0);

    const vec3 shading
        = normalAt(corner.x) * weight.x + normalAt(corner.y) * weight.y + normalAt(corner.z) * weight.z;
    const vec3 normal = dot(shading, shading) > 1e-8 ? normalize(mat3(toWorld) * shading) : surface.mGeometric;

    // **Which side the ray met is the plane's answer, and the shading normal is not allowed to give
    // a different one.** Morrowind's vertex normals are authored coarsely enough to point clean
    // through their own triangle: a stretch of the floor in the Seyda Neen customs office
    // interpolates to one aimed at the ground, on a quad whose plane is level to a hundredth. Turned
    // to face the *ray* that normal is left pointing down — it already does face a camera looking
    // along the floor — and a floor with a normal under it drops every lamp overhead on the cosine
    // and sends its bounce into itself. That is the black band, and it slid about as the camera
    // moved because which way a bad normal is turned depended on where the eye was.
    //
    // So the plane is turned to the ray first, and the shading normal is brought to the side it
    // names. **Sheets still light from both faces**, which is what the turn is for at all: a
    // tapestry met from behind has its plane turned toward the ray like anything else, and its
    // normal follows. And the winding drops out — flipping it flips the plane, which the turn
    // undoes — so the two hundredths of a percent of triangles wound against their own normals are
    // not a case this has to be right about.
    const vec3 facing = faceforward(surface.mGeometric, direction, surface.mGeometric);
    surface.mNormal = dot(normal, facing) < 0.0 ? -normal : normal;

    const GpuMaterial material = materials[instance.mMaterial];
    surface.mWater = material.mKind == KIND_WATER;
    surface.mEmissiveColour = material.mEmissiveColour;

    vec2 uv[3];
    triangleUvs(corner, uv);

    vec3 albedo = NO_TEXTURE_ALBEDO;

    // **Ground that kept its stack**, which is every chunk near enough to be worth the sharpness.
    // A chunk wide enough to be distant had the whole stack flattened into one texture in its own
    // coordinates instead, and falls through to the single fetch below — which is what it now is.
    // `Rtx::sCompositeFrom` is where the two swap over.
    if (material.mKind == KIND_TERRAIN && material.mDiffuse == NO_TEXTURE)
    {
        // Each layer is a tiling texture masked by its own grid of weights, and the stack sums to
        // one where the masks were built to — the same sum the rasterizer reaches by drawing the
        // layers over each other with additive blending and one pass apiece.
        albedo = vec3(0.0);
        const vec2 chunkUv = interpolate(uv, weight);
        for (uint i = 0u; i < material.mLayerCount; ++i)
        {
            const GpuLayer layer = layers[material.mLayerOffset + i];
            const float showing = maskWeight(layer, chunkUv);
            if (showing <= 0.0)
                continue;

            albedo += showing
                * sampleAlbedo(
                    layer.mDiffuse, uv, weight, layer.mDiffuseTransform, crossed, direction, surface.mFootprint);
        }
    }
    else if (material.mDiffuse != NO_TEXTURE)
    {
        albedo = sampleAlbedo(
            material.mDiffuse, uv, weight, material.mTextureTransform, crossed, direction, surface.mFootprint);
    }
    surface.mAlbedo = albedo * material.mDiffuseColour.rgb;

    if (material.mEmissive != NO_TEXTURE)
        surface.mEmitted = EMISSIVE_INTENSITY
            * sampleDiffuse(
                material.mEmissive, uv, weight, material.mTextureTransform, crossed, direction, surface.mFootprint)
                  .rgb;

    return surface;
}

#endif
