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

/// Whether a material is meant to be seen through everywhere, rather than in the holes of a mask.
///
/// One number and no mode, for the reason `GpuMaterial::mOpacity` gives: a leaf card and a pane of
/// glass carry the same alpha mode, and the host is what tells them apart.
bool isTranslucent(GpuMaterial material)
{
    return material.mOpacity < 1.0;
}

/// Whether a material carries a mask a ray is tested against: a cutoff, a texture to read it off,
/// and no translucency — a pane is there everywhere, thinly, and has no holes to find.
///
/// The host's `Material::isCutout`, asked again here because the build marks an instance by it
/// and the shader must agree about which candidates it meant.
bool hasMask(GpuMaterial material)
{
    return !isTranslucent(material) && material.mAlphaCutoff > 0.0 && material.mDiffuse != NO_TEXTURE;
}

/// How much of a surface is there, before its texture is read.
///
/// **The material's own alpha and the placement's fade**, which are two different facts: every
/// placement of a model shares its material, and a fade belongs to one actor. See
/// `Rtx::MeshInstance::mOpacity`.
float surfaceOpacity(GpuInstance instance, GpuMaterial material)
{
    return instance.mOpacity * material.mOpacity;
}

/// Whether what is behind a surface of this opacity is meant to show through it.
///
/// The product rather than the two facts it is made of, so a caller that wants the number as well as
/// the answer makes it once.
bool isSeenThrough(float opacity)
{
    return opacity < 1.0;
}

/// How much of a see-through surface is there, where a ray met it.
///
/// **The texture's alpha over the two `surfaceOpacity` carries**, which is what a blend does: the
/// texture says where a stained pane's lead is, the material says how much glass there is, and the
/// placement says how much of the whole thing the game is showing.
///
/// **One function, so that one surface cannot be hazed two ways.** A shadow ray asks what it let
/// past and the eye asks what it covered, which are the same number seen from either side — and they
/// stay the same number only while there is one place it is worked out.
///
/// **Guarded against a material with no mask**, which `candidateStops` says more about: a surface is
/// see-through on its alpha alone, and an untextured pane is all glass and no lead.
///
/// @param opacity what `surfaceOpacity` gave for this hit, made once by the caller.
float sampledOpacity(
    float opacity, GpuMaterial material, vec2 uv[3], vec3 weight, vec3 crossed, vec3 direction, float coneWidth)
{
    const float covered = material.mDiffuse == NO_TEXTURE
        ? 1.0
        : sampleDiffuse(material.mDiffuse, uv, weight, material.mTextureTransform, crossed, direction, coneWidth).a;

    return clamp(covered * opacity, 0.0, 1.0);
}

/// Whether a candidate hit stops the ray, and what it lets past where it does not.
///
/// **One load of the instance and its material, for what were three questions asked in turn.** A
/// shadow ray used to ask whether a candidate was see-through, then — of whichever answer came
/// back — either what it let past or whether it landed in a hole, and each of those three read the
/// instance and the material again. They are one decision about one surface, so they are one load.
///
/// Only instances the build marked non-opaque reach this, and it marks them for three different
/// reasons: a mask to test, a material's own alpha, and a placement the game is fading. Just the
/// first has anything to read here, which is what the early answer below is for. Past it there is no
/// mode to branch on — the comparison is the whole test, and a surface that wants none stores a
/// threshold nothing can fail.
///
/// The level the mask reads at matters as much as the test does. A mask point-sampled at its finest
/// mip answers for one texel out of the hundreds a distant pixel covers, and a binary test on that
/// is a coin toss per pixel — a canopy comes back as speckle, and it crawls as the camera moves.
/// Letting the cone average the mask first costs a leaf edge some of its bite, which is by a long
/// way the better of the two errors.
///
/// @param through multiplied by what a see-through candidate let past. Untouched otherwise, which
///        the compiler folds away with `seeThrough`.
/// @param seeThrough whether a see-through candidate is walked past or taken against its cutoff like
///        any other. **A literal at every call**, so the whole branch folds.
bool candidateStops(uint instanceIndex, uint primitive, vec2 bary, vec3 crossed, vec3 direction, float coneWidth,
    bool seeThrough, inout float through)
{
    const GpuInstance instance = instances[instanceIndex];
    const GpuMaterial material = materials[instance.mMaterial];

    const float opacity = surfaceOpacity(instance, material);
    const bool walkPast = seeThrough && isSeenThrough(opacity);

    // **Met and not tested where there is nothing to test.** A material with no mask arrives here
    // because forcing an instance non-opaque says nothing about its material: a pane of glass is
    // forced for its own alpha, and an actor is forced for the fade its placement carries. Neither
    // promises a mask, and a texture nothing bound must not be read for one.
    //
    // **The material and not the placement.** An actor the game is fading keeps every hole in its
    // mask, because a fade is not a hole — what the fade does to what is left is measured elsewhere.
    if (!walkPast && !hasMask(material))
        return true;

    vec2 uv[3];
    triangleUvs(triangleCorners(meshes[instance.mMesh], primitive), uv);
    const vec3 weight = cornerWeights(bary);

    if (walkPast)
    {
        through *= 1.0 - sampledOpacity(opacity, material, uv, weight, crossed, direction, coneWidth);
        return false;
    }

    return sampleDiffuse(material.mDiffuse, uv, weight, material.mTextureTransform, crossed, direction, coneWidth).a
        >= material.mAlphaCutoff;
}

/// The candidate loop, run to completion. It confirms every hit that lands on the material rather
/// than in one of its holes, and — where the caller asks — walks past a translucent one instead,
/// keeping what it let through.
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
/// @param through,seeThrough handed straight to `candidateStops`, which says what each is for. A
///        ray that sees through cannot commit the surface it saw through, so a caller with no use
///        for `through` must say false and get the surface. Only the shadow ray says true today —
///        its answer is a product, and a product does not care what order its factors arrived in.
#define RTX_RESOLVE(query, along, cone, through, seeThrough)                                                \
    while (rayQueryProceedEXT(query))                                                                       \
    {                                                                                                       \
        if (rayQueryGetIntersectionTypeEXT(query, false) != gl_RayQueryCandidateIntersectionTriangleEXT)    \
            continue;                                                                                       \
                                                                                                            \
        const uint candidateInstance = rayQueryGetIntersectionInstanceCustomIndexEXT(query, false);         \
        const uint candidatePrimitive = rayQueryGetIntersectionPrimitiveIndexEXT(query, false);             \
        const vec2 candidateBary = rayQueryGetIntersectionBarycentricsEXT(query, false);                    \
                                                                                                            \
        vec3 candidateCorners[3];                                                                           \
        rayQueryGetIntersectionTriangleVertexPositionsEXT(query, false, candidateCorners);                  \
        const vec3 candidateCross                                                                           \
            = triangleCross(candidateCorners, rayQueryGetIntersectionObjectToWorldEXT(query, false));       \
                                                                                                            \
        if (candidateStops(candidateInstance, candidatePrimitive, candidateBary, candidateCross, (along),   \
                (cone), (seeThrough), (through)))                                                           \
            rayQueryConfirmIntersectionEXT(query);                                                          \
    }

/// How much of a light `reach` away along `towards` reaches `from`.
///
/// No cone here, so the cutout is decided at the finest mip. A shadow ray carries no footprint, and
/// aliasing in a leaf's shadow is worth far less than aliasing on the leaf.
///
/// **A ray shorter than the bias it starts past is not a ray.** A candle sitting a unit off a table
/// asks for a shadow ray whose end is behind its own beginning, and `rayQueryInitializeEXT` with a
/// `tmax` under its `tmin` is undefined — which is a hang or a garbage answer rather than an empty
/// one. Nothing fits in that gap anyway: the bias is what a hit point's own surface needs to be
/// clear of, so a light inside it is a light nothing can stand between.
/// **A translucent surface dims the light rather than stopping it**, and the order it is met in does
/// not matter: the answer is a product, and a product does not care. That is what makes the shadow
/// the cheap half of transparency — the eye needs its layers sorted and this needs nothing at all.
///
/// **`TerminateOnFirstHit` stays.** A translucent candidate is never confirmed, so traversal walks
/// past it and keeps the early out for the first thing that does stop the ray.
float lightThrough(vec3 from, vec3 towards, float distance)
{
    if (distance <= SHADOW_BIAS)
        return 1.0;

    float through = 1.0;

    rayQueryEXT query;
    rayQueryInitializeEXT(
        query, sceneTop, gl_RayFlagsTerminateOnFirstHitEXT, MASK_SOLID, from, SHADOW_BIAS, towards, distance);
    RTX_RESOLVE(query, towards, 0.0, through, true)

    if (rayQueryGetIntersectionTypeEXT(query, true) != gl_RayQueryCommittedIntersectionNoneEXT)
        return 0.0;

    return through;
}

/// How far the nearest solid surface is along a ray, at most `reach` away.
///
/// **Traversal and the cutout, and no material resolved at all.** An asker that wants a distance
/// pays for the whole of `trace` otherwise — the plane, the shading normal, the emissive, and for a
/// piece of ground the entire layer stack with a mask read apiece — to read one float back off it.
/// The cutout still runs, or the ray would stop in the holes of a mask.
///
/// **`reach` is the answer as well as the limit**, which is what makes such a ray short: an asker
/// that only cares whether anything stands within a band hands over the band, and reads a miss as
/// *no nearer than that*. Nothing here runs to `mFar` unless a caller asks it to.
float solidWithin(vec3 origin, vec3 direction, float tmin, float reach, float footprint, float spread)
{
    rayQueryEXT query;
    rayQueryInitializeEXT(query, sceneTop, gl_RayFlagsNoneEXT, MASK_SOLID, origin, tmin, direction, reach);

    // An lvalue the macro needs and nothing here reads: this ray sees through nothing, so what a
    // translucent surface would have let past is never accumulated.
    float passed = 1.0;
    RTX_RESOLVE(query, direction, footprint + spread * rayQueryGetIntersectionTEXT(query, false), passed, false)

    if (rayQueryGetIntersectionTypeEXT(query, true) == gl_RayQueryCommittedIntersectionNoneEXT)
        return reach;

    return rayQueryGetIntersectionTEXT(query, true);
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

    /// The triangle's own plane, turned the same way `mNormal` is.
    ///
    /// **What every question about *sides* asks, because the interpolated normal cannot answer
    /// one.** A shading normal on this content routinely leans past its own triangle — four hits in
    /// a hundred by more than sixty degrees — so it says a light behind the surface is in front of
    /// it, and it aims a bounce into the floor the bounce left. The plane says neither. It is turned
    /// rather than left as the winding wound it so that a caller has one vector meaning "out of this
    /// surface" and no side of its own to work out.
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

    /// How much of this surface is there, where the ray met it. One for everything that is all
    /// there, which is nearly everything.
    ///
    /// `sampledOpacity`, which is what a shadow ray asks of the same surface through
    /// `candidateStops` — so the two cannot haze one surface two ways.
    float mOpacity;

    /// What light on the far side of this surface is worth to the side the ray met, against the
    /// same light on the near side. Nought for everything solid; `SHEET_TRANSMISSION` for a leaf.
    ///
    /// **Two facts and neither alone is a leaf.** The mesh says the content doubled it for its
    /// back — `GpuMesh::mSheet` — and the material says it carries a mask. A tabard is doubled and
    /// has none, and is cloth lit from the side it is seen from; a pane carries a mask and is not
    /// doubled, and passes light by its opacity rather than by this.
    float mTransmission;
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
    surface.mOpacity = 1.0;
    surface.mTransmission = 0.0;

    rayQueryEXT query;
    // No blanket opaque flag: the per-instance bits the build set from each material are what decide
    // whether traversal stops to ask, and forcing opacity here would override them and put every
    // leaf back inside the card it was painted on.
    rayQueryInitializeEXT(query, sceneTop, gl_RayFlagsNoneEXT, mask, origin, tmin, direction, frame.mFar);

    // The eye keeps nothing it passed through yet, so it takes every candidate against its cutoff —
    // which is what leaves a pane of glass drawn as the half of its mask that survives one.
    float passed = 1.0;
    RTX_RESOLVE(query, direction, footprint + spread * rayQueryGetIntersectionTEXT(query, false), passed, false)

    if (rayQueryGetIntersectionTypeEXT(query, true) == gl_RayQueryCommittedIntersectionNoneEXT)
        return surface;

    surface.mHit = true;
    surface.mDistance = rayQueryGetIntersectionTEXT(query, true);
    surface.mPosition = origin + direction * surface.mDistance;

    surface.mFootprint = footprint + spread * surface.mDistance;

    surface.mInstance = rayQueryGetIntersectionInstanceCustomIndexEXT(query, true);

    const GpuInstance instance = instances[surface.mInstance];
    const GpuMesh mesh = meshes[instance.mMesh];
    const uvec3 corner = triangleCorners(mesh, rayQueryGetIntersectionPrimitiveIndexEXT(query, true));
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
    surface.mGeometric = faceforward(surface.mGeometric, direction, surface.mGeometric);
    surface.mNormal = dot(normal, surface.mGeometric) < 0.0 ? -normal : normal;

    const GpuMaterial material = materials[instance.mMaterial];
    surface.mWater = material.mKind == KIND_WATER;
    surface.mEmissiveColour = material.mEmissiveColour;

    surface.mTransmission = mesh.mSheet != 0u && hasMask(material) ? SHEET_TRANSMISSION : 0.0;

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

    // **Fetched again rather than kept from the albedo.** `sampleAlbedo` drops the alpha on purpose,
    // for the reason written over it: it is the hottest sampler in the shader and an out-parameter
    // there costs every opaque surface in the frame. This is a fetch a pane of glass pays and
    // nothing else does.
    const float opacity = surfaceOpacity(instance, material);
    if (isSeenThrough(opacity))
        surface.mOpacity = sampledOpacity(opacity, material, uv, weight, crossed, direction, surface.mFootprint);

    if (material.mEmissive != NO_TEXTURE)
        surface.mEmitted = EMISSIVE_INTENSITY
            * sampleDiffuse(
                material.mEmissive, uv, weight, material.mTextureTransform, crossed, direction, surface.mFootprint)
                  .rgb;

    return surface;
}

#endif
