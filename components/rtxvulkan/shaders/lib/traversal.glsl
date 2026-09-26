#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_TRAVERSAL_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_TRAVERSAL_GLSL

// Traversal, and what a ray found resolved down to the inputs shading needs.
//
// **No light here.** That is what lets water shade by tracing again: a reflection's hit is
// resolved by this same `trace` and shaded by `shadeSurface`, and neither calls back into
// water — which a shader with no recursion could not survive.

// A candidate's and a committed hit's corners come out of the query itself, which is what a
// traversal needs to build the plane without a vertex buffer bound for it.
#extension GL_EXT_ray_tracing_position_fetch : require

#include "look.h"
#include "scene.h"
#include "basis.glsl"
#include "bindings.glsl"
#include "geometry.glsl"
#include "ground.glsl"
#include "texturing.glsl"
#include "variants.glsl"

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

/// Whether a material carries a mask a ray is tested against: a cutoff, and no translucency — a
/// pane is there everywhere, thinly, and has no holes to find.
///
/// The host's `Material::isCutout`, asked again here because the build marks an instance by it
/// and the shader must agree about which candidates it meant. That rule refuses a cutoff with no
/// diffuse, so the diffuse is not asked again here.
bool hasMask(GpuMaterial material)
{
    return !isTranslucent(material) && material.mAlphaCutoff > 0.0;
}

/// Whether a material is a medium the ray goes through rather than a surface it can stop on.
///
/// The host's `Material::isMedium`, decided there because the measurement behind it is a walk over
/// every texel of a texture. `MATERIAL_MEDIUM` is the bit it writes.
bool isMedium(GpuMaterial material)
{
    return (material.mFlags & MATERIAL_MEDIUM) != 0u;
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

/// Which faces traversal shows a ray, from whether the ray draws the picture.
///
/// **A ray that draws shows what the rasterizer shows.** OpenMW draws the world with
/// `GL_CULL_FACE` on, so the content states which face of a surface is there, and a ray standing in
/// for that pass states it the same way. A ray that carries light meets a surface from either
/// side instead: a wall met from behind that stopped nothing would leak the light behind it.
///
/// `SceneAcceleration::placeRow` is where a row says it is drawn from both faces, and
/// `InstanceRecord::mTwoSided` is what the content said.
uint facingFor(bool draws)
{
    return draws ? gl_RayFlagsCullBackFacingTrianglesEXT : gl_RayFlagsNoneEXT;
}

/// Whether what is behind a surface of this opacity is meant to show through it.
///
/// The product rather than the two facts it is made of, so a caller that wants the number as well as
/// the answer makes it once.
bool isSeenThrough(float opacity)
{
    return opacity < 1.0;
}

/// How squarely a shading normal has to face the ray that found the surface before it is tilted back
/// toward one that faces it more, `facingRay`: a mapped normal toward the interpolated one, and the
/// lobe's normal toward the plane. Small, as the water's is — a guard against a normal leaning past
/// the ray, and not a limit on the map.
const float SHADING_MIN_FACING = 0.03;

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
/// An untextured pane is all glass and no lead: its diffuse is `TEXTURE_NEUTRAL`, whose one texel
/// has an alpha of one, so nothing here asks whether there is a texture.
///
/// @param opacity what `surfaceOpacity` gave for this hit, made once by the caller.
/// @param painted the texture's own alpha where the ray met it.
float sampledOpacity(float opacity, float painted)
{
    return clamp(painted * opacity, 0.0, 1.0);
}

/// The same, for a caller that has not read the texture yet — which is every caller but the one
/// that wants the colour beside the alpha. `gatherAlong` is that one.
///
/// @param point where the hit lands on the material's own texture, made once by the caller.
float sampledOpacity(float opacity, GpuMaterial material, TexturePoint point)
{
    return sampledOpacity(opacity, sampleDiffuse(material.mDiffuse, point).a);
}

/// One, in the units an order-free sum over candidates is taken in: twenty fractional bits.
///
/// **A sum over candidates is an integer sum, because the order candidates arrive in is the
/// card's.** The specification's "Ray Intersection Candidate Determination" says *there is no
/// ordering guarantee between operations performed on different intersection candidates*, and a
/// float sum or product rounds differently for every order it is taken in — so a shadow made of
/// three panes came out one bit different from run to run, and the frame hash with it. An integer
/// sum is the same sum in every order. Twenty bits, because the largest term is a colour times a
/// coverage and the sums saturate at four thousand of those, which no stack of shells reaches; and
/// it holds a shadow's logarithm to a relative part in a million.
///
/// **Saturating, so an overflow is a clamp and not a wrap**: `addShare` is the one way a share is
/// summed.
const float SHARE_UNIT = 1048576.0;

/// A term of an order-free sum, off a non-negative float.
uint sharePart(float part)
{
    return uint(round(max(part, 0.0) * SHARE_UNIT));
}

uvec3 sharePart(vec3 part)
{
    return uvec3(round(max(part, vec3(0.0)) * SHARE_UNIT));
}

/// `sharePart` undone, for a sum read back as a float.
float shareTotal(uint total)
{
    return float(total) / SHARE_UNIT;
}

uint addShare(uint total, uint part)
{
    return total + min(part, ~total);
}

uvec3 addShare(uvec3 total, uvec3 part)
{
    return total + min(part, ~total);
}

/// What a see-through candidate keeps from a ray, as the term an order-free sum carries: the
/// logarithm of what it lets past, so that the product of the surfaces is the sum of the terms.
///
/// **Clamped at twenty-four binary orders**, which is a surface nothing measurable gets through
/// and the finest step an eight-bit alpha can name, so that an opaque texel is a large finite term
/// and not an infinity the sum could not hold.
uint blockedBy(float opacity)
{
    return sharePart(-log2(max(1.0 - opacity, exp2(-24.0))));
}

/// What a sum of `blockedBy` terms lets through.
float throughBlocked(uint blocked)
{
    return exp2(-shareTotal(blocked));
}

/// Where on its material's texture a candidate was crossed, at the level `coneWidth` resolves: what
/// the cutout test reads its alpha through and a medium's crossing its texel.
TexturePoint candidatePoint(
    uvec3 corners, GpuMaterial material, vec2 bary, vec3 crossed, vec3 direction, float coneWidth)
{
    vec2 uv[3];
    triangleUvs(corners, uv);
    return texturePoint(uv, bary, material.mTextureTransform, surfaceConeAt(crossed, direction), coneWidth);
}

/// Whether a candidate hit stops the ray, and what it lets past where it does not.
///
/// **One load of the instance and its material, and not three questions asked in turn.** Whether a
/// candidate is see-through, what it lets past and whether it landed in a hole are one decision
/// about one surface, so they are one load.
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
/// @param blocked raised by what a see-through candidate kept, in `blockedBy`'s terms. Untouched
///        otherwise, which the compiler folds away with `seeThrough`.
/// @param seeThrough whether a see-through candidate is walked past or taken against its cutoff like
///        any other. **A literal at every call**, so the whole branch folds.
bool candidateStops(uint instanceIndex, uint primitive, vec2 bary, vec3 crossed, vec3 direction, float coneWidth,
    bool seeThrough, inout uint blocked)
{
    const GpuInstance instance = instanceAt(instanceIndex);
    const GpuMaterial material = materialAt(instance.mMaterial);

    const float opacity = surfaceOpacity(instance, material);
    const bool walkPast = seeThrough && isSeenThrough(opacity);

    // **Nothing stops on a medium, whatever the ray was asking.** A surface that is nowhere opaque
    // is not a surface: the eye walks through a cloud and commits the mountain behind it, and a
    // bounce does the same. What the cloud looks like is `mediumAlong`'s answer and not a hit's.
    //
    // **Before the mask test and before any texture is read**, because a ray that is not keeping
    // what it passed through has nothing to read one for.
    if (!walkPast && isMedium(material))
        return false;

    // **Met and not tested where there is nothing to test.** A material with no mask arrives here
    // because forcing an instance non-opaque says nothing about its material: a pane of glass is
    // forced for its own alpha, and an actor is forced for the fade its placement carries. Neither
    // promises a mask.
    //
    // **The material and not the placement.** An actor the game is fading keeps every hole in its
    // mask, because a fade is not a hole — what the fade does to what is left is measured elsewhere.
    if (!walkPast && !hasMask(material))
        return true;

    const TexturePoint point = candidatePoint(
        triangleCorners(meshAt(instance.mMesh), primitive), material, bary, crossed, direction, coneWidth);

    if (walkPast)
    {
        blocked = addShare(blocked, blockedBy(sampledOpacity(opacity, material, point)));
        return false;
    }

    return sampleDiffuse(material.mDiffuse, point).a >= material.mAlphaCutoff;
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
/// @param blocked,seeThrough handed straight to `candidateStops`, which says what each is for. A
///        ray that sees through cannot commit the surface it saw through, so a caller with no use
///        for `blocked` must say false and get the surface. The shadow ray says true — it wants a
///        sum, and the sum does not depend on the order they arrived in.
#define RTX_RESOLVE(query, along, cone, blocked, seeThrough)                                                \
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
                (cone), (seeThrough), (blocked)))                                                           \
            rayQueryConfirmIntersectionEXT(query);                                                          \
    }

/// What a traversal answered, before anything at all is read off it.
///
/// **Geometry the query alone can give**: no instance row, no mesh, no vertex and no material.
/// Everything a hit *leads to* is read by `resolve`, off the tables this names.
struct Hit
{
    bool mHit;

    /// Which row of the instance table this came off, which is the custom index the build wrote and
    /// not the structure's own.
    uint mInstance;

    /// Which row of the mesh table the instance wears, read off the instance here for the corners
    /// and carried out so `resolve` reads the mesh row once and not the instance's for it again.
    uint mMesh;

    /// Where in the shared vertex buffers this triangle's three corners are, already global.
    ///
    /// **Resolved inside the query and carried out, rather than the primitive index.** The index
    /// block has to be read there in any case, to interpolate the shading normal while the
    /// object-to-world matrix is still in scope, and `resolve` would read the same block for the
    /// same three numbers. Two words more in a `Hit` against one block-table address and three
    /// index loads on every ray that lands — which is neutral in time, and kept for the
    /// duplication.
    uvec3 mCorner;

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
    /// **Three floats where the transform they came through is nine.** The object-to-world matrix
    /// is only ever used to bring this one vector across, so the vector is what a `Hit` carries and
    /// the matrix is not. Not unit: `resolve` normalises it, and the uniform scale in the transform
    /// drops out there.
    vec3 mShading;

    /// The vertex tangent interpolated across the triangle, in world space, with the bitangent's
    /// handedness in `w` — or nought where the mesh carries none, which is every vanilla mesh.
    /// Brought across here for the reason `mShading` is: the matrix does not survive the call.
    vec4 mTangent;
};

/// A ray that committed nothing, as far away as anything can be.
Hit noHit()
{
    Hit hit;
    hit.mHit = false;
    hit.mInstance = 0u;
    hit.mMesh = 0u;
    hit.mCorner = uvec3(0u);
    hit.mBary = vec2(0.0);
    hit.mDistance = frame.mReach;
    hit.mFootprint = 0.0;
    hit.mCrossed = vec3(0.0);
    hit.mShading = vec3(0.0);
    hit.mTangent = vec4(0.0);

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
    hit.mBary = bary;
    hit.mDistance = distance;
    hit.mFootprint = footprint;
    hit.mCrossed = triangleCross(corners, toWorld);

    // **The one vertex fetch a traversal does, and it is here so that the transform need not
    // survive the call.** The test is on the mesh's own normal rather than on the transformed one:
    // a mesh with no normals stores zeros, and a scale that shrank a real normal past the threshold
    // would otherwise change which branch it took.
    const GpuInstance placement = instanceAt(instance);
    hit.mMesh = placement.mMesh;
    const GpuMesh mesh = meshAt(hit.mMesh);
    hit.mCorner = triangleCorners(mesh, primitive);

    const vec3 shading = triangleNormal(hit.mCorner, bary);
    hit.mShading = dot(shading, shading) > 1e-8 ? mat3(toWorld) * shading : vec3(0.0);

    // **Three more words, and only off a mesh that carries them.** The bit is on the row this already
    // read, so a hit on anything no normal map is read through — every hit in a vanilla scene —
    // pays one test and no fetch.
    hit.mTangent = vec4(0.0);
    if (HAS_MAPS && (mesh.mShape & MESH_TANGENTS) != 0u)
    {
        const vec4 tangent = triangleTangent(hit.mCorner, bary);
        hit.mTangent = vec4(mat3(toWorld) * tangent.xyz, tangent.w);
    }

    return hit;
}

/// How much of a light `reach` away along `towards` reaches `from`.
///
/// No cone here, so the cutout is decided at the finest mip. A shadow ray carries no footprint, and
/// aliasing in a leaf's shadow is worth far less than aliasing on the leaf.
///
/// **And handing it one loses.** JCGT 10(1) 2021 finds level zero slower than a cone level in every
/// scene it tries, but the paper's finding is about the *fetch*, and this path's cost is the
/// *level*: a width of nought is answered at once, so level zero here skips the texel count's load
/// in `coneLod`, and a determinant and two logarithms in `coneBase`, at every candidate. What
/// those early returns save is more than the cache gives back, on every place tried.
///
/// **A ray shorter than the bias it starts past is not a ray.** A candle sitting a unit off a table
/// asks for a shadow ray whose end is behind its own beginning, and `rayQueryInitializeEXT` with a
/// `tmax` under its `tmin` is undefined — which is a hang or a garbage answer rather than an empty
/// one. Nothing fits in that gap anyway: the bias is what a hit point's own surface needs to be
/// clear of, so a light inside it is a light nothing can stand between.
/// **A translucent surface dims the light rather than stopping it**, and the order it is met in does
/// not matter: the answer is a product, and a product does not care. That is what makes the shadow
/// the cheap half of transparency — the eye needs its layers sorted and this needs nothing at all.
/// Taken as `blockedBy`'s sum and not as the product itself, because a float product does care.
///
/// **`TerminateOnFirstHit` stays.** A translucent candidate is never confirmed, so traversal walks
/// past it and keeps the early out for the first thing that does stop the ray.
float lightThrough(vec3 from, vec3 towards, float distance)
{
    if (distance <= SHADOW_BIAS)
        return 1.0;

    uint blocked = 0u;

    rayQueryEXT query;
    rayQueryInitializeEXT(
        query, sceneTop, gl_RayFlagsTerminateOnFirstHitEXT, solidMask(frame.mRayMask), from, SHADOW_BIAS, towards,
        distance);
    RTX_RESOLVE(query, towards, 0.0, blocked, true)

    if (rayQueryGetIntersectionTypeEXT(query, true) != gl_RayQueryCommittedIntersectionNoneEXT)
        return 0.0;

    return throughBlocked(blocked);
}

/// How far the nearest surface that stops a ray is along it, at most `reach` away.
///
/// **Traversal and the cutout, and no material resolved at all.** An asker that wants a distance
/// pays for the whole of `trace` otherwise — the plane, the shading normal, the emissive, and for a
/// piece of ground the entire layer stack with a mask read apiece — to read one float back off it.
/// The cutout still runs, or the ray would stop in the holes of a mask.
///
/// **`reach` is the answer as well as the limit**, which is what makes such a ray short: an asker
/// that only cares whether anything stands within a band hands over the band, and reads a miss as
/// *no nearer than that*. Nothing here runs to `mReach` unless a caller asks it to.
///
/// @param mask which surfaces stop the ray. `solidWithin` asks for solids alone.
/// @param seeThrough whether a surface the eye would see through is walked past rather than stopped
///        at, which is the eye's own rule: `visibility.rgen` peels those and commits what stands
///        behind them. An asker whose question is "where does the picture end" wants this, and one
///        asking "what is the nearest thing there" does not.
/// @param draws the same division again, and the same two askers — `facingFor`. Where the picture
///        ends is where the eye's own ray ends, and the eye culls.
float surfaceWithin(vec3 origin, vec3 direction, float tmin, float reach, float footprint, float spread, uint mask,
    bool seeThrough, bool draws)
{
    rayQueryEXT query;
    rayQueryInitializeEXT(query, sceneTop, facingFor(draws), mask, origin, tmin, direction, reach);

    // An lvalue the macro needs and nothing here reads: what a surface walked past let through is
    // a question for whoever wants the picture, and this ray wants the distance.
    uint blocked = 0u;
    RTX_RESOLVE(query, direction, footprint + spread * rayQueryGetIntersectionTEXT(query, false), blocked, seeThrough)

    if (rayQueryGetIntersectionTypeEXT(query, true) == gl_RayQueryCommittedIntersectionNoneEXT)
        return reach;

    return rayQueryGetIntersectionTEXT(query, true);
}

float solidWithin(vec3 origin, vec3 direction, float tmin, float reach, float footprint, float spread)
{
    return surfaceWithin(origin, direction, tmin, reach, footprint, spread, solidMask(frame.mRayMask), false, false);
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
    /// content routinely points through its own triangle. Where the material has a normal map, the
    /// map's normal, turned the same way and tilted to face the ray.
    vec3 mNormal;

    /// The interpolated normal, turned as `mNormal` is, before any map: `mNormal` wherever there is
    /// no map. **What a closed shape takes a light's side from**, so a normal map cannot move a
    /// light to the other side of the surface — `litCosine` says why the side is not the map's
    /// question.
    vec3 mSmooth;

    /// Which way the ray that found this surface was travelling, which is what the lobe is
    /// evaluated against: the eye's own ray, a bounce, a reflection.
    vec3 mIncident;

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

    /// The diffuse albedo: the texture's, delit, for a vanilla surface, and `(1 - metal)` of the
    /// authored base colour for one with a specular map.
    vec3 mAlbedo;

    /// The reflectance at normal incidence, `F0`: nought for a vanilla surface, which reflects
    /// nothing — `DIELECTRIC_F0` says why — and the base colour's mix toward `DIELECTRIC_F0` by the
    /// map's metalness for one with a specular map.
    vec3 mSpecular;

    /// The perceptual roughness the map paints, and one where there is no map — which is what a
    /// Lambert surface is to the upscaler, and what `lambertResponse` reports.
    float mRoughness;

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
    /// back — `MESH_SHEET` in `GpuMesh::mShape` — and the material says it carries a mask. A
    /// tabard is doubled and has none, and is cloth lit from the side it is seen from; a pane
    /// carries a mask and is not doubled, and passes light by its opacity rather than by this.
    float mTransmission;
};

/// A ray that met nothing, as far away as anything can be: what `resolveFor` answers with for a
/// miss, and what it fills in from for a hit.
Surface noSurface(vec3 origin)
{
    Surface surface;
    surface.mHit = false;
    surface.mGround = false;
    surface.mPosition = origin;
    surface.mNormal = vec3(0.0, 0.0, 1.0);
    surface.mSmooth = vec3(0.0, 0.0, 1.0);
    surface.mIncident = vec3(0.0, 0.0, -1.0);
    surface.mGeometric = vec3(0.0, 0.0, 1.0);
    surface.mAlbedo = vec3(0.0);
    surface.mSpecular = vec3(0.0);
    surface.mRoughness = 1.0;
    surface.mEmissiveColour = vec3(0.0);
    surface.mEmitted = vec3(0.0);
    surface.mDistance = frame.mReach;
    surface.mInstance = 0u;
    surface.mFootprint = 0.0;
    surface.mOpacity = 1.0;
    surface.mClosed = false;
    surface.mTransmission = 0.0;

    return surface;
}

/// What a hit is made of.
///
/// **Everything a hit leads to and nothing the traversal already answered.** Every table this reads
/// is keyed on where the ray landed — the instance, its mesh, its material, its textures — and
/// `Hit` is what the traversal answered.
///
/// @param layered whether ground that kept its layer stack can reach this hit. **A literal at every
///        call**, so the stack's loop and the four tables it walks are compiled out of a shader no
///        such hit can arrive at. A closest-hit shader is picked by the instance's own material
///        kind, so the two that are not terrain's know the answer is no — register relief no
///        driver here will report a number for.
/// @param detailed whether the hit is a picture: the eye's own, a reflection's, the bed under a
///        waterline — `trace`'s `draws`. **A diffuse bounce's far hit is not**, and reads no normal
///        map and no parallax: what it sends back is averaged over a hemisphere and then filtered,
///        and relief read there moved nothing a 1024-frame reference could tell from its own noise,
///        at Balmora or in the census office. The albedo, the reflectance and the roughness are read
///        either way, because the energy the hit sends back is theirs.
Surface resolveFor(Hit hit, vec3 origin, vec3 direction, bool layered, bool detailed)
{
    Surface surface = noSurface(origin);

    if (!hit.mHit)
        return surface;

    surface.mHit = true;
    surface.mDistance = hit.mDistance;
    surface.mPosition = origin + direction * surface.mDistance;
    surface.mIncident = direction;

    surface.mFootprint = hit.mFootprint;

    surface.mInstance = hit.mInstance;

    const GpuInstance instance = instanceAt(surface.mInstance);
    const GpuMesh mesh = meshAt(hit.mMesh);
    const uvec3 corner = hit.mCorner;

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
    const bool turned = dot(normal, surface.mGeometric) < 0.0;
    surface.mNormal = turned ? -normal : normal;
    surface.mSmooth = surface.mNormal;

    const GpuMaterial material = materialAt(instance.mMaterial);
    surface.mGround = layered && material.mLayerCount > 0u;

    // **Fetched for every hit, and selected between without a branch.** A mesh that brought no
    // colour holds white, so the load answers neutrally rather than needing a case, and the two
    // weights below are one or nought — the content states which, and `MATERIAL_VERTEX_TINT` says
    // why the mode does not survive the trip. Over half of Morrowind's shapes and every piece of
    // ground carry a colour, so a branch would be taken by most of the frame anyway.
    const vec3 vertexColour = triangleColour(corner, hit.mBary);
    const float tinted = float((material.mFlags & MATERIAL_VERTEX_TINT) != 0u);
    const float glowing = float((material.mFlags & MATERIAL_VERTEX_GLOW) != 0u);

    surface.mEmissiveColour = mix(material.mEmissiveColour, vertexColour, glowing);

    surface.mClosed = (mesh.mShape & MESH_CLOSED) != 0u;
    surface.mTransmission = (mesh.mShape & MESH_SHEET) != 0u && hasMask(material) ? SHEET_TRANSMISSION : 0.0;

    vec2 uv[3];
    triangleUvs(corner, uv);

    // Where the hit lands on the material's own sheet, which the albedo, the opacity and the
    // emissive map all read at. A terrain layer has a transform of its own and makes its own.
    TexturePoint point = texturePoint(uv, hit.mBary, material.mTextureTransform, cone, surface.mFootprint);

    // **A normal map, read through the tangents the mesh carries**, in the frame `normals.glsl`
    // builds: the tangent unit, the bitangent `cross(N, T) * w`, and the interpolated normal, with
    // no orthogonalisation between them — which is what the maps were checked against. Built on
    // the normal as the mesh states it and then turned with it, so a sheet met from behind sees
    // the map's relief from behind. A mesh the map reached with no tangents keeps its normal.
    if (HAS_MAPS && detailed && holdsTexture(material.mNormal) && dot(hit.mTangent.xyz, hit.mTangent.xyz) > 0.0)
    {
        const vec3 tangent = normalize(hit.mTangent.xyz);
        const vec3 bitangent = cross(normal, tangent) * hit.mTangent.w;

        // **Shifted toward the eye by the map's height first**, where it carries one, so every read
        // on this sheet after it lands where the relief puts it: the map itself, the albedo, the
        // opacity, and the specular, emissive and dark maps. The rasterizer shifts its diffuse and
        // its normal map alone, because its other maps may read another set of coordinates; these
        // read the set this point is on, and a specular map left behind is roughness painted for
        // another texel of the colour. The height is read where the point was, as `objects.frag`
        // reads it, and the shift runs along the axes the relief is painted on.
        if ((material.mFlags & MATERIAL_PARALLAX) != 0u)
        {
            const vec3 eye = vec3(dot(tangent, -direction), dot(bitangent, -direction), dot(normal, -direction));
            point.mAt += parallaxShift(eye, sampleDiffuse(material.mNormal, point).a);
        }

        const vec3 painted = sampleNormalMap(material.mNormal, point);
        const vec3 mapped = normalize(tangent * painted.x + bitangent * painted.y + normal * painted.z);

        surface.mNormal = facingRay(turned ? -mapped : mapped, surface.mSmooth, direction, SHADING_MIN_FACING);
    }

    // **Ground that kept its stack**, which is every chunk near enough to be worth the sharpness,
    // and one fetch for everything else, an untextured surface included: its diffuse is
    // `TEXTURE_NEUTRAL`, which reads as the grey it always read as. A chunk outside the active grid
    // had the whole stack flattened into one texture in its own coordinates instead, by
    // `groundcomposite.comp` over the same sum as this, and is that one fetch too.
    // `CellPlacer::wantsFlattening` is where the two swap over, and `MATERIAL_STACKED` is what the
    // row says about which it is.
    vec3 albedo = vec3(0.0);

    // What a stack's maps add, each weighted as the layer's albedo is: the share of the weight on
    // layers that reflect, the roughness over every layer with a Lambert one at one, and the
    // tangent-space normals — the layers' own where they have a map and straight up where not.
    // `groundcomposite.comp` sums the first two the same way into a distant chunk's gloss.
    float weights = 0.0;
    float reflecting = 0.0;
    float roughness = 0.0;
    vec3 painted = vec3(0.0);
    bool relief = false;

    // **A chunk whose composite stands in is summed from its stack**, which is still the scene's: the
    // composites are what give way first where the device runs out of room, and the grey stand-in
    // would be the whole chunk.
    if (layered
        && ((material.mFlags & MATERIAL_STACKED) != 0u
            || (material.mLayerCount > 0u && !holdsTexture(material.mDiffuse))))
    {
        // Each layer is a tiling texture masked by its own grid of weights, and the stack sums to
        // one where the masks were built to — the same sum the rasterizer reaches by drawing the
        // layers over each other with additive blending and one pass apiece.
        const vec2 chunkUv = acrossTriangle(uv[0], uv[1], uv[2], hit.mBary);

        // The frame `terrain.vert` gives every layer: the chunk's x, which is the world's, the
        // bitangent `cross(N, x)`, and the tangent x less its part along N. A heightfield's normal
        // leans up, so x is never along it. The eye in it is what a layer with a height is shifted
        // by, the frame being one for every layer.
        //
        // **Filled behind `HAS_MAPS` and not declared with it**: the optimizer the kernels are
        // compared through keeps arithmetic nothing reads, so a frame computed outside the branch
        // would be in every vanilla program, read by nothing and moving its digest by a normalize.
        vec3 layerTangent = vec3(0.0);
        vec3 layerBitangent = vec3(0.0);
        vec3 layerEye = vec3(0.0);
        if (HAS_MAPS && detailed)
        {
            const vec3 across = vec3(1.0, 0.0, 0.0);
            layerTangent = normalize(across - normal * dot(normal, across));
            layerBitangent = cross(normal, across);
            layerEye = vec3(dot(layerTangent, -direction), dot(layerBitangent, -direction), dot(normal, -direction));
        }

        for (uint i = 0u; i < material.mLayerCount; ++i)
        {
            const GpuLayer layer = layerAt(material.mLayerOffset + i);
            const float showing = maskWeight(layer, chunkUv);
            if (showing <= 0.0)
                continue;

            // Shifted as `terrain.frag` shifts the layer, before any read of it, by the height read
            // where the layer was.
            TexturePoint at = texturePoint(uv, hit.mBary, layer.mDiffuseTransform, cone, surface.mFootprint);
            if (HAS_MAPS && detailed && (layer.mFlags & LAYER_PARALLAX) != 0u && holdsTexture(layer.mNormal))
                at.mAt += parallaxShift(layerEye, sampleDiffuse(layer.mNormal, at).a);

            const bool authored = HAS_MAPS && layerAuthored(layer, sceneTexels());
            const vec4 shown = layerTexel(layer, at.mAt, coneLod(layer.mDiffuse, at), frame.mDelight, authored);
            albedo += showing * shown.rgb;

            if (HAS_MAPS)
            {
                weights += showing;
                roughness += showing * shown.a;
                if (authored)
                    reflecting += showing;

                const bool mapped = detailed && holdsTexture(layer.mNormal);
                painted += showing * (mapped ? sampleNormalMap(layer.mNormal, at) : vec3(0.0, 0.0, 1.0));
                relief = relief || mapped;
            }
        }

        // **The layers' normals summed by their weights and then carried once**, through the
        // layers' one frame: summing before the frame is summing after it. The rasterizer lights
        // each layer under its own normal and blends the light; one lobe under the blended normal
        // is what a path tracer can afford, and it parts from that only where the masks blend.
        if (HAS_MAPS && relief)
        {
            const vec3 mapped
                = normalize(layerTangent * painted.x + layerBitangent * painted.y + normal * painted.z);

            surface.mNormal = facingRay(turned ? -mapped : mapped, surface.mSmooth, direction, SHADING_MIN_FACING);
        }
    }
    else if (HAS_MAPS && holdsTexture(material.mSpecular))
        albedo = sampleDiffuse(material.mDiffuse, point).rgb;
    else
        albedo = sampleAlbedo(material.mDiffuse, point);

    // The vertex colour *replaces* the material's tint where the content asked for it, which is
    // what `glColorMaterial(GL_AMBIENT_AND_DIFFUSE)` does and what `getDiffuseColor` reads in the
    // game's own shader. Multiplying the two together would tint a surface twice.
    const vec3 tint = mix(material.mDiffuseColour, vertexColour, tinted);
    surface.mAlbedo = albedo * tint;

    // **A specular map says the diffuse was authored as a base colour**, which is why it was read
    // above with nothing divided out of it: glTF's metal and roughness, the base colour split
    // between what a metal reflects and what a dielectric scatters.
    //
    // **The tint darkens the lobe as well as the base**, because on this content it is mostly light
    // baked into the vertices rather than a colour — Wareya's `PBR_VERTEX_COLOR_HACK`, which these
    // maps were made against, puts it on the light for the same reason. Left off the lobe, a 4%
    // reflectance hazed over every darkened surface: a tapestry in the Seyda Neen census office
    // reflected two to four times what it scattered. On the reflectance at normal incidence, it
    // darkens the edge too, below `SPECULAR_EDGE_SCALE`'s two per cent, which is the specular
    // occlusion that convention is for.
    //
    // **Ground reflects as a dielectric, over the share of it that reflects at all** — a stack from
    // its layers, a distant chunk from the gloss baked beside its composite — and the tint darkens
    // it for the same reason. A stack with no layer that reflects keeps the Lambert surface's
    // numbers exactly, since no division is taken for it.
    if (HAS_MAPS && reflecting > 0.0)
    {
        surface.mSpecular = vec3(DIELECTRIC_F0 * (reflecting / weights)) * tint;
        surface.mRoughness = roughness / weights;
    }
    else if (HAS_MAPS && holdsTexture(material.mSpecular) && surface.mGround)
    {
        const vec2 gloss = sampleSpecularMap(material.mSpecular, point);
        surface.mSpecular = vec3(DIELECTRIC_F0 * gloss.x) * tint;
        surface.mRoughness = gloss.y;
    }
    else if (HAS_MAPS && holdsTexture(material.mSpecular))
    {
        const vec2 painted = sampleSpecularMap(material.mSpecular, point);
        surface.mSpecular = mix(vec3(DIELECTRIC_F0), albedo, painted.x) * tint;
        surface.mRoughness = painted.y;
        surface.mAlbedo *= 1.0 - painted.x;
    }

    // **Fetched again rather than kept from the albedo.** `sampleAlbedo` drops the alpha on purpose,
    // for the reason written over it: it is the hottest sampler in the shader and an out-parameter
    // there costs every opaque surface in the frame. This is a fetch a pane of glass pays and
    // nothing else does.
    const float opacity = surfaceOpacity(instance, material);
    if (isSeenThrough(opacity))
        surface.mOpacity = sampledOpacity(opacity, material, point);

    // **The dark map multiplies the whole of it**, colour and alpha, which is where `objects.frag`
    // puts it. At the unit the content bound it at, on whichever set that unit reads: the Sixth
    // House banners read their second set and the durzog its first.
    if (holdsTexture(material.mDark))
    {
        const uint unit = (material.mFlags >> MATERIAL_DARK_UNIT_SHIFT) & MATERIAL_DARK_UNIT_MASK;
        TexturePoint darkPoint = point;
        if (readsSecondUvs(mesh, unit))
        {
            vec2 second[3];
            triangleSecondUvs(mesh, corner, second);
            darkPoint = texturePoint(second, hit.mBary, vec4(1.0, 1.0, 0.0, 0.0), cone, surface.mFootprint);
        }

        const vec4 dark = sampleDiffuse(material.mDark, darkPoint);
        surface.mAlbedo *= dark.rgb;

        // And the lobe, for the tint's reason: a dark map is light painted in.
        if (HAS_MAPS)
            surface.mSpecular *= dark.rgb;

        // The alpha only where an alpha is read at all: an opaque surface's is never written to
        // the frame, and the peel reads `mOpacity` as whether there is a layer to peel.
        if (isSeenThrough(opacity))
            surface.mOpacity *= dark.a;
    }

    if (holdsTexture(material.mEmissive))
        surface.mEmitted = EMISSIVE_INTENSITY * sampleDiffuse(material.mEmissive, point).rgb;

    // **A sphere-mapped sheet, added past the albedo and indexed by where the eye is.** The
    // original adds `envMap` after its lighting, so it is emission that depends on the view: the
    // violet sheet a magic effect wears and the caustic sheet an enchanted item shimmers with are
    // what the artist drew, and neither is a reflection of anything. The coordinates are the
    // rasterizer's own — `objects.vert` reflects the eye-space view vector about the eye-space
    // normal and folds it onto the sheet — in the frame camera's basis, so a bounce that lands on
    // glass armour sees the sheet the way the reflection camera would.
    if (holdsTexture(material.mEnvironment))
    {
        // The camera's axes are scaled by the image plane's half extents and are taken unit here;
        // the eye space is OpenGL's, looking down its own -Z.
        const vec3 right = normalize(frame.mCamera.mRight);
        const vec3 up = normalize(frame.mCamera.mUp);
        const vec3 forward = frame.mCamera.mForward;
        const vec3 viewEye = vec3(dot(direction, right), dot(direction, up), -dot(direction, forward));
        const vec3 normalEye = vec3(dot(surface.mNormal, right), dot(surface.mNormal, up), -dot(surface.mNormal, forward));

        // **Its own footprint and not the surface's**, which `spherePoint` says at length: a sheet
        // indexed by the reflection is not read at the level the mesh's coordinates ask for. The
        // second fetch of the triangle's normals in the frame, and the only one outside the
        // traversal — paid by the materials that wear a sheet, which are few.
        vec3 normal[3];
        triangleNormals(corner, normal);

        const TexturePoint sheet = spherePoint(normal, normalEye, viewEye, cone, surface.mFootprint);

        surface.mEmitted
            += SUNLIT_WHITE * sampleDiffuse(material.mEnvironment, sheet).rgb * material.mEnvironmentColour;
    }

    return surface;
}

/// The same, for a ray that could have landed on anything. Every inline query in the frame — a
/// bounce, a reflection, the bed under a waterline pixel — is one of these.
///
/// @param draws whether the ray draws the picture, which is whether the hit is `detailed`.
Surface resolve(Hit hit, vec3 origin, vec3 direction, bool draws)
{
    return resolveFor(hit, origin, direction, true, draws);
}

/// Traverses, and answers with what the query committed.
///
/// @param footprint how wide the ray's cone starts, which for a primary ray is nothing and for a
///        reflection is whatever the pixel had already spread to at the water.
/// @param spread how much wider that cone gets per unit travelled.
/// @param draws whether this ray draws the picture — `facingFor`. A reflection and the bed under a
///        waterline pixel do; a bounce carries light and does not.
Hit traverse(vec3 origin, vec3 direction, float tmin, float footprint, float spread, uint mask, bool draws)
{
    // No blanket opaque flag: the per-instance bits the build set from each material are what decide
    // whether traversal stops to ask, and forcing opacity here would override them and put every leaf
    // back inside the card it was painted on.
    rayQueryEXT query;
    rayQueryInitializeEXT(query, sceneTop, facingFor(draws), mask, origin, tmin, direction, frame.mReach);

    // An lvalue the resolve needs and nothing here reads: a ray that keeps what it passed through
    // cannot commit the surface it passed through, and this one commits.
    uint blocked = 0u;
    RTX_RESOLVE(query, direction, footprint + spread * rayQueryGetIntersectionTEXT(query, false), blocked, false)

    if (rayQueryGetIntersectionTypeEXT(query, true) == gl_RayQueryCommittedIntersectionNoneEXT)
        return noHit();

    vec3 corners[3];
    rayQueryGetIntersectionTriangleVertexPositionsEXT(query, true, corners);

    const float distance = rayQueryGetIntersectionTEXT(query, true);
    return committedHit(rayQueryGetIntersectionInstanceCustomIndexEXT(query, true),
        rayQueryGetIntersectionPrimitiveIndexEXT(query, true), rayQueryGetIntersectionBarycentricsEXT(query, true),
        distance, footprint + spread * distance, corners, rayQueryGetIntersectionObjectToWorldEXT(query, true));
}

/// Traverses, and resolves whatever it hit.
///
/// **The two halves back to back, for every ray but the eye's own.** Only the primary ray has
/// anything to put between them, and `visibility.rgen` is where it does.
Surface trace(vec3 origin, vec3 direction, float tmin, float footprint, float spread, uint mask, bool draws)
{
    return resolve(traverse(origin, direction, tmin, footprint, spread, mask, draws), origin, direction, draws);
}

#endif
