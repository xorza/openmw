// `#pragma once` everywhere else in this tree, and an include guard here for the reason
// `components/rtx/shaders/portable.h` gives.
#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_TRAVERSAL_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_TRAVERSAL_GLSL

// Traversal, and what a ray found resolved down to the inputs shading needs.
//
// **No light here.** That is what lets water shade by tracing again: a reflection's hit is
// resolved by this same `trace` and shaded by `shadeSurface`, and neither calls back into
// water — which a shader with no recursion could not survive.

#include "look.h"
#include "scene.h"
#include "bindings.glsl"
#include "geometry.glsl"
#include "texturing.glsl"

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
/// @param point where the hit lands on the material's own texture, made once by the caller too.
float sampledOpacity(float opacity, GpuMaterial material, TexturePoint point, SurfaceCone cone, float coneWidth)
{
    const float covered
        = material.mDiffuse == NO_TEXTURE ? 1.0 : sampleDiffuse(material.mDiffuse, point, cone, coneWidth).a;

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
    const GpuInstance instance = instanceAt(instanceIndex);
    const GpuMaterial material = materialAt(instance.mMaterial);

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
    triangleUvs(triangleCorners(meshAt(instance.mMesh), primitive), uv);
    const TexturePoint point = texturePoint(uv, cornerWeights(bary), material.mTextureTransform);
    const SurfaceCone cone = surfaceConeAt(crossed, direction);

    if (walkPast)
    {
        through *= 1.0 - sampledOpacity(opacity, material, point, cone, coneWidth);
        return false;
    }

    return sampleDiffuse(material.mDiffuse, point, cone, coneWidth).a >= material.mAlphaCutoff;
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

/// What a traversal answered, before anything at all is read off it.
///
/// **Geometry the query alone can give**: no instance row, no mesh, no vertex and no material. That
/// is what makes this the record a reorder is sorted on — everything a hit *leads to* is read after
/// the threads have been regrouped, where the lanes of a warp are asking about the same instance.
struct Hit
{
    bool mHit;

    /// Which row of the instance table this came off, which is the custom index the build wrote and
    /// not the structure's own.
    uint mInstance;

    uint mPrimitive;
    vec2 mBary;
    float mDistance;

    /// How wide the ray's cone was where it landed.
    float mFootprint;

    /// Twice the area of the triangle, as a vector along its plane's normal, in world space.
    ///
    /// **Made here rather than carried as three corners**, because the corners come out of the query
    /// through position fetch and nothing past the traversal wants them: one cross product is three
    /// floats where they are nine, and `resolve` needs only what the cross says.
    vec3 mCrossed;

    /// The vertex normal interpolated across the triangle, in world space — or nought where the
    /// mesh carries none, which `resolve` reads as "use the plane".
    ///
    /// **Three floats where the transform they came through is nine.** Live state is what a reorder
    /// costs, and the object-to-world matrix is only ever used to bring this one vector across, so
    /// the vector is what survives the call and the matrix does not. Not unit: `resolve` normalises
    /// it, and the uniform scale in the transform drops out there.
    vec3 mShading;
};

/// A ray that committed nothing, as far away as anything can be.
Hit noHit()
{
    Hit hit;
    hit.mHit = false;
    hit.mInstance = 0u;
    hit.mPrimitive = 0u;
    hit.mBary = vec2(0.0);
    hit.mDistance = frame.mFar;
    hit.mFootprint = 0.0;
    hit.mCrossed = vec3(0.0);
    hit.mShading = vec3(0.0);

    return hit;
}

/// The committed intersection, read off the query and put into world space.
///
/// @param corners the triangle as position fetch gave it, in the mesh's own space.
Hit committedHit(
    uint instance, uint primitive, vec2 bary, float distance, float footprint, vec3 corners[3], mat4x3 toWorld)
{
    Hit hit;
    hit.mHit = true;
    hit.mInstance = instance;
    hit.mPrimitive = primitive;
    hit.mBary = bary;
    hit.mDistance = distance;
    hit.mFootprint = footprint;
    hit.mCrossed = triangleCross(corners, toWorld);

    // **The one vertex fetch a traversal does, and it is here so that the transform need not
    // survive the call.** The test is on the mesh's own normal rather than on the transformed one,
    // which is the decision `resolve` used to make: a mesh with no normals stores zeros, and a
    // scale that shrank a real normal past the threshold would otherwise change which branch it
    // took.
    const GpuInstance placement = instanceAt(instance);
    const vec3 shading = triangleNormal(triangleCorners(meshAt(placement.mMesh), primitive), cornerWeights(bary));
    hit.mShading = dot(shading, shading) > 1e-8 ? mat3(toWorld) * shading : vec3(0.0);

    return hit;
}

/// A traversal run to completion, with whatever it committed read into `hit`.
///
/// **A macro for the reason `RTX_RESOLVE` is one, and for one more.** `glslc` refuses a `rayQueryEXT`
/// as a parameter, so a traversal cannot be handed to a function — and a ray generation shader has to
/// reach the same query again afterwards to record its hit object out of it, which no
/// `out hitObjectEXT` could carry back either, because that type may not be a parameter. So the body
/// is written once here and expanded at the two places that need it.
///
/// @param query an uninitialised traversal, which this leaves committed so that a caller may record
///        a hit object from it.
/// @param hit a `Hit` this fills in.
/// @param footprint,spread how wide the ray's cone starts and how fast it opens. See `trace`.
#define RTX_TRAVERSE(query, hit, origin, direction, tmin, footprint, spread, mask)                          \
    {                                                                                                       \
        /* No blanket opaque flag: the per-instance bits the build set from each material are what   */      \
        /* decide whether traversal stops to ask, and forcing opacity here would override them and   */      \
        /* put every leaf back inside the card it was painted on.                                    */      \
        rayQueryInitializeEXT(                                                                              \
            (query), sceneTop, gl_RayFlagsNoneEXT, (mask), (origin), (tmin), (direction), frame.mFar);      \
                                                                                                            \
        /* An lvalue the resolve needs and nothing here reads: a ray that keeps what it passed       */      \
        /* through cannot commit the surface it passed through, and this one commits.                */      \
        float traversedThrough = 1.0;                                                                       \
        RTX_RESOLVE((query), (direction),                                                                   \
            (footprint) + (spread) * rayQueryGetIntersectionTEXT((query), false), traversedThrough, false)  \
                                                                                                            \
        if (rayQueryGetIntersectionTypeEXT((query), true) == gl_RayQueryCommittedIntersectionNoneEXT)       \
            (hit) = noHit();                                                                                \
        else                                                                                                \
        {                                                                                                   \
            vec3 traversedCorners[3];                                                                       \
            rayQueryGetIntersectionTriangleVertexPositionsEXT((query), true, traversedCorners);             \
                                                                                                            \
            const float traversedDistance = rayQueryGetIntersectionTEXT((query), true);                     \
            (hit) = committedHit(rayQueryGetIntersectionInstanceCustomIndexEXT((query), true),              \
                rayQueryGetIntersectionPrimitiveIndexEXT((query), true),                                    \
                rayQueryGetIntersectionBarycentricsEXT((query), true), traversedDistance,                   \
                (footprint) + (spread) * traversedDistance, traversedCorners,                               \
                rayQueryGetIntersectionObjectToWorldEXT((query), true));                                    \
        }                                                                                                   \
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
///
/// **Four states for every ray, the fog volume's probes and the ambient rays included.** Folding
/// their unknown microtriangles to two with `gl_RayFlagsForceOpacityMicromap2StateEXT`, as Indiana
/// Jones traces its indirect rays, measured five microseconds off `air` on every view and two
/// hundred onto the trace at Vivec, whose banners and lattices the micromap does nothing for.
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
///
/// @param mask which surfaces stop the ray. `solidWithin` asks for solids alone.
float surfaceWithin(vec3 origin, vec3 direction, float tmin, float reach, float footprint, float spread, uint mask)
{
    rayQueryEXT query;
    rayQueryInitializeEXT(query, sceneTop, gl_RayFlagsNoneEXT, mask, origin, tmin, direction, reach);

    // An lvalue the macro needs and nothing here reads: this ray sees through nothing, so what a
    // translucent surface would have let past is never accumulated.
    float passed = 1.0;
    RTX_RESOLVE(query, direction, footprint + spread * rayQueryGetIntersectionTEXT(query, false), passed, false)

    if (rayQueryGetIntersectionTypeEXT(query, true) == gl_RayQueryCommittedIntersectionNoneEXT)
        return reach;

    return rayQueryGetIntersectionTEXT(query, true);
}

float solidWithin(vec3 origin, vec3 direction, float tmin, float reach, float footprint, float spread)
{
    return surfaceWithin(origin, direction, tmin, reach, footprint, spread, MASK_SOLID);
}

/// What a ray found, resolved down to the inputs shading needs.
///
/// Geometry and material only — no light. That is what lets water shade by tracing again: the
/// reflection's hit is resolved by this same function and shaded by `shadeSurface`, and neither
/// calls back into water, which a shader with no recursion could not survive.
struct Surface
{
    bool mHit;

    /// Whether what the ray met is the ground itself rather than something standing on it.
    ///
    /// **What `BOUNCE_REACH` is allowed to hand the sky.** A draw about open ground reaches it
    /// whatever stands nearby; the same draw about a wall spends half of itself on whatever the wall
    /// belongs to. `layered` folds it to false in the two shaders no terrain can reach, so the
    /// escape and the reach compile out of them entirely.
    bool mGround;

    vec3 mPosition;

    /// The shading normal, turned to the side of the triangle's plane the ray arrived on.
    /// Morrowind's sheet geometry is lit from both faces, so which side that is carries no meaning
    /// of its own — and the *plane* is what turns it, never the interpolated normal, which on this
    /// content routinely points through its own triangle.
    vec3 mNormal;

    /// The triangle's own plane, turned the same way `mNormal` is.
    ///
    /// **What a bounce is bounded by, and what an open surface takes a light's side from.** A
    /// shading normal on this content routinely leans past its own triangle — four hits in a hundred
    /// by more than sixty degrees — so it aims a bounce into the floor the bounce left, and on
    /// anything with no far side it says a light behind the surface is in front of it. The plane
    /// says neither. `mClosed` is what decides whether a light's side is its question or the
    /// normal's; a bounce is always its. It is turned rather than left as the winding wound it so
    /// that a caller has one vector meaning "out of this surface" and no side of its own to work
    /// out.
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

    /// Whether every edge of this mesh carries a triangle each way — `MESH_CLOSED`.
    ///
    /// **It says which of the two normals above is lying.** A closed shape is a solid the content
    /// faceted, so its interpolated normal describes it and its triangles do not: a light the normal
    /// faces is one whole facets turn away from, and reading the side off the plane costs those
    /// facets every scrap of direct light in a patch with the triangle's own edges. An open shape is
    /// the reverse — a quad whose normals lean off it — and reading the side off the normal lights it
    /// from behind, because there is no far side for the shadow ray to stop in.
    bool mClosed;

    /// What light on the far side of this surface is worth to the side the ray met, against the
    /// same light on the near side. Nought for everything solid; `SHEET_TRANSMISSION` for a leaf.
    ///
    /// **Two facts and neither alone is a leaf.** The mesh says the content doubled it for its
    /// back — `GpuMesh::mSheet` — and the material says it carries a mask. A tabard is doubled and
    /// has none, and is cloth lit from the side it is seen from; a pane carries a mask and is not
    /// doubled, and passes light by its opacity rather than by this.
    float mTransmission;
};

/// What a hit is made of, once the threads that share one have been put in the same warp.
///
/// **Everything a hit leads to and nothing the traversal already answered.** Every table this reads
/// is keyed on where the ray landed — the instance, its mesh, its material, its textures — so this
/// is the half of the old `trace` that a reorder is there to make coherent, and `Hit` is the half
/// that has to survive the call.
///
/// @param layered whether ground that kept its layer stack can reach this hit. **A literal at every
///        call**, so the stack's loop and the four tables it walks are compiled out of a shader no
///        such hit can arrive at. A closest-hit shader is picked by the instance's own material
///        kind, so the two that are not terrain's know the answer is no — which is the register
///        relief Stage 2 is for, and which no driver here will report a number for.
Surface resolveFor(Hit hit, vec3 origin, vec3 direction, bool layered)
{
    Surface surface;
    surface.mHit = false;
    surface.mGround = false;
    surface.mPosition = origin;
    surface.mNormal = vec3(0.0, 0.0, 1.0);
    surface.mGeometric = vec3(0.0, 0.0, 1.0);
    surface.mAlbedo = vec3(0.0);
    surface.mEmissiveColour = vec3(0.0);
    surface.mEmitted = vec3(0.0);
    surface.mDistance = frame.mFar;
    surface.mFootprint = 0.0;
    surface.mOpacity = 1.0;
    surface.mClosed = false;
    surface.mTransmission = 0.0;

    if (!hit.mHit)
        return surface;

    surface.mHit = true;
    surface.mDistance = hit.mDistance;
    surface.mPosition = origin + direction * surface.mDistance;

    surface.mFootprint = hit.mFootprint;

    surface.mInstance = hit.mInstance;

    const GpuInstance instance = instanceAt(surface.mInstance);
    const GpuMesh mesh = meshAt(instance.mMesh);
    const uvec3 corner = triangleCorners(mesh, hit.mPrimitive);
    const vec3 weight = cornerWeights(hit.mBary);

    // The plane the traversal already gave: position fetch has the corners and no buffer has to be
    // bound for them, where the vertices' own normals are a fetch and are better where they are.
    const vec3 crossed = hit.mCrossed;
    surface.mGeometric = dot(crossed, crossed) > 0.0 ? normalize(crossed) : vec3(0.0, 0.0, 1.0);

    // Every texture read below shares this hit's triangle and this ray: a chunk's whole layer stack,
    // the opacity a pane pays for, and the emissive map.
    const SurfaceCone cone = surfaceConeAt(crossed, direction);

    const vec3 normal = dot(hit.mShading, hit.mShading) > 0.0 ? normalize(hit.mShading) : surface.mGeometric;

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

    const GpuMaterial material = materialAt(instance.mMaterial);
    surface.mGround = layered && material.mLayerCount > 0u;
    surface.mEmissiveColour = material.mEmissiveColour;

    surface.mClosed = (mesh.mShape & MESH_CLOSED) != 0u;
    surface.mTransmission = (mesh.mShape & MESH_SHEET) != 0u && hasMask(material) ? SHEET_TRANSMISSION : 0.0;

    vec2 uv[3];
    triangleUvs(corner, uv);

    // Where the hit lands on the material's own sheet, which the albedo, the opacity and the
    // emissive map all read at. A terrain layer has a transform of its own and makes its own.
    const TexturePoint point = texturePoint(uv, weight, material.mTextureTransform);

    vec3 albedo = NO_TEXTURE_ALBEDO;

    // **Ground that kept its stack**, which is every chunk near enough to be worth the sharpness.
    // A chunk wide enough to be distant had the whole stack flattened into one texture in its own
    // coordinates instead, and falls through to the single fetch below — which is what it now is.
    // `Rtx::sCompositeFrom` is where the two swap over.
    if (layered && material.mLayerCount > 0u && material.mDiffuse == NO_TEXTURE)
    {
        // Each layer is a tiling texture masked by its own grid of weights, and the stack sums to
        // one where the masks were built to — the same sum the rasterizer reaches by drawing the
        // layers over each other with additive blending and one pass apiece.
        albedo = vec3(0.0);
        const vec2 chunkUv = interpolate(uv, weight);
        for (uint i = 0u; i < material.mLayerCount; ++i)
        {
            const GpuLayer layer = layerAt(material.mLayerOffset + i);
            const float showing = maskWeight(layer, chunkUv);
            if (showing <= 0.0)
                continue;

            albedo += showing
                * sampleAlbedo(
                    layer.mDiffuse, texturePoint(uv, weight, layer.mDiffuseTransform), cone, surface.mFootprint);
        }
    }
    else if (material.mDiffuse != NO_TEXTURE)
    {
        albedo = sampleAlbedo(material.mDiffuse, point, cone, surface.mFootprint);
    }
    surface.mAlbedo = albedo * material.mDiffuseColour;

    // **Fetched again rather than kept from the albedo.** `sampleAlbedo` drops the alpha on purpose,
    // for the reason written over it: it is the hottest sampler in the shader and an out-parameter
    // there costs every opaque surface in the frame. This is a fetch a pane of glass pays and
    // nothing else does.
    const float opacity = surfaceOpacity(instance, material);
    if (isSeenThrough(opacity))
        surface.mOpacity = sampledOpacity(opacity, material, point, cone, surface.mFootprint);

    if (material.mEmissive != NO_TEXTURE)
        surface.mEmitted
            = EMISSIVE_INTENSITY * sampleDiffuse(material.mEmissive, point, cone, surface.mFootprint).rgb;

    return surface;
}

/// The same, for a ray that could have landed on anything. Every inline query in the frame — a
/// bounce, a reflection, the bed under a waterline pixel — is one of these.
Surface resolve(Hit hit, vec3 origin, vec3 direction)
{
    return resolveFor(hit, origin, direction, true);
}

/// Traverses, and answers with what the query committed.
///
/// @param footprint how wide the ray's cone starts, which for a primary ray is nothing and for a
///        reflection is whatever the pixel had already spread to at the water.
/// @param spread how much wider that cone gets per unit travelled.
Hit traverse(vec3 origin, vec3 direction, float tmin, float footprint, float spread, uint mask)
{
    rayQueryEXT query;
    Hit hit;
    RTX_TRAVERSE(query, hit, origin, direction, tmin, footprint, spread, mask)

    return hit;
}

/// Traverses, and resolves whatever it hit.
///
/// **The two halves back to back, for every ray but the eye's own.** Only the primary ray has
/// anything to put between them, and `visibility.rgen` is where it does.
Surface trace(vec3 origin, vec3 direction, float tmin, float footprint, float spread, uint mask)
{
    return resolve(traverse(origin, direction, tmin, footprint, spread, mask), origin, direction);
}

#endif
