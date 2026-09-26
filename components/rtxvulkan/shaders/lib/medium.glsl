#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_MEDIUM_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_MEDIUM_GLSL

// The shells of a cloud the content modelled as geometry, gathered along the eye's ray as one
// medium rather than met one surface at a time.
//
// **What this is for.** `meshes/f/active_blight_large.nif` is eleven alpha shells over one another,
// and neither of its textures holds a single opaque texel. A ray tracer that peels the nearest of
// those and paints the next one as though it were opaque draws a cloud as a flat red sheet.
//
// **One traversal for the whole stack.** The eye's own ray walks past every medium it meets —
// `candidateStops` is where that is decided — so it commits the mountain behind the cloud and this
// walk gathers what stands in front of it, on a mask no other instance carries. Re-tracing per layer
// is the alternative, and Anagnostou measures it at an order of magnitude over the same picture
// alpha-blended by a rasterizer. This walk costs a fraction of the trace under a cloud and nothing
// in a cell with none.
//
// Kostas Anagnostou, *Raytraced Order Independent Transparency*:
// https://interplayoflight.wordpress.com/2023/07/15/raytraced-order-independent-transparency/
//
// **Thickness rather than coverage** is Umenhoffer and Szirmay-Kalos's, whose spherical billboards
// measure how much medium a ray crosses rather than how much of a pixel a card fills:
// https://www.semanticscholar.org/paper/7a249dbd873f02248eecc3ebf811b71ea5e90277
//
// **Order-independent, because there is no order to be had.** A candidate loop meets the shells in
// whatever order the structure hands them over, so they are composited by what they mean rather than
// by depth: the exact total coverage `1 - prod(1 - a)`, filled with the coverage-weighted mean
// colour and taken at the coverage-weighted depth. That is exact for one shell and for any number of
// shells of one colour, which is what a cloud is.

#include "colour.h"
#include "look.h"
#include "scene.h"
#include "bindings.glsl"
#include "fog.glsl"
#include "frame.glsl"
#include "geometry.glsl"
// A medium is lit the way a puff of smoke is and fills the layer a puff of smoke fills — `puffLight`
// and `PuffLayer` are the one place each of those is said, and this walk is the second caller.
#include "sprites.glsl"
#include "texturing.glsl"
#include "traversal.glsl"

/// What one crossing of a shell hides, out of what its texture painted.
///
/// **A texel says what one crossing square to the shell takes**, so a slanted crossing goes `1/cos`
/// as far through the same slab — which is `paintedOver` with a secant for its count. Head on this
/// is the number the author tuned, exactly; away from it the shell thickens the way a real one does,
/// which is what makes a cloud read as a body rather than as a stack of decals.
///
/// `MEDIUM_GRAZE_LIMIT` says why the secant is clamped.
///
/// @param facing how square the ray is to the shell, `|dot(normal, direction)|`.
float mediumCrossing(float painted, float facing)
{
    return paintedOver(painted, 1.0 / max(facing, 1.0 / MEDIUM_GRAZE_LIMIT));
}

/// One crossing of a walk that confirms nothing, read down to what the walk weighs it by: the rows
/// the candidate names, the corners of its triangle and where between them the walk crossed, and
/// the texel read there.
struct Crossing
{
    GpuInstance mInstance;
    GpuMaterial mMaterial;
    uvec3 mCorner;
    vec2 mBary;
    vec4 mTexel;
};

/// The crossing a candidate names.
///
/// @param coneWidth how wide the ray's cone is at the crossing, which picks the texel's level.
Crossing crossingOf(uint instanceIndex, uint primitive, vec2 bary, vec3 crossed, vec3 direction, float coneWidth)
{
    Crossing crossing;
    crossing.mInstance = instanceAt(instanceIndex);
    crossing.mMaterial = materialAt(crossing.mInstance.mMaterial);
    crossing.mCorner = triangleCorners(meshAt(crossing.mInstance.mMesh), primitive);
    crossing.mBary = bary;

    const TexturePoint point
        = candidatePoint(crossing.mCorner, crossing.mMaterial, crossing.mBary, crossed, direction, coneWidth);

    // One path: an untextured shell names `TEXTURE_NEUTRAL`, which reads as the grey it stood for.
    crossing.mTexel = sampleDiffuse(crossing.mMaterial.mDiffuse, point);
    return crossing;
}

/// How a walk of candidates weighs what it crosses: which class it is cast against, and what of a
/// crossing it reads. **A literal at every call**, so what a rule folds out is dead code and not a
/// branch — the two walks below are one loop under two of these.
struct GatherRule
{
    /// The one class the walk is cast against: `MASK_MEDIUM` or `MASK_ADDITIVE`.
    uint mMask;

    /// Whether a shell is thickened by the angle the ray crosses it at — `mediumCrossing` — or
    /// taken at its painted alpha, as a sheet that adds is.
    bool mThickens;

    /// Whether `MATERIAL_ADD_WHOLE` is honoured: a `ONE, ONE` sheet reads no alpha at all.
    bool mWholeAlpha;

    /// Whether the vertex colour selects the tint and the glow, as `resolveFor` reads it. A cloud
    /// takes its material's own; an effect's sheet takes what the content asked for.
    bool mVertexTint;

    /// Whether what the crossings let through is summed — `blockedBy` — which a layer that covers
    /// reports as its transmittance and a layer that adds has no use for.
    bool mBlocks;

    /// Whether this walk draws the picture, which is what decides the faces it is shown —
    /// `facingFor`. A sheet that adds is drawn. A medium is entered and left, and which face of a
    /// shell its winding names carries no meaning on that content.
    bool mDraws;
};

/// What a walk gathered along a ray.
///
/// **In `sharePart`'s units, because the crossings arrive in the card's order** — `SHARE_UNIT`
/// says what a float sum taken in that order did to the frame hash. The distance is summed as a
/// share of the limit, which is the one bound a crossing has.
struct Gathered
{
    /// The crossings' colour under their tint, and their glow, each weighted by what it hid.
    uvec3 mUnlit;
    uvec3 mGlowed;

    uint mCoverage;
    uint mCoveredAt;
    uint mBlocked;

    /// The plane of the crossing that hid the most, which is the one side the layer is given,
    /// how much it hid, how far off it was and which instance it was.
    vec3 mCoveringNormal;
    float mCoveringAlpha;
    float mCoveringAt;
    uint mCovering;
};

/// Every surface of `rule.mMask` the eye crosses before `limit`, gathered.
///
/// **Nothing is ever confirmed here.** What the walk gathers is not a surface, so it runs to
/// `limit` and every crossing on the way is kept; the loop ends because the candidates do.
///
/// **Order-independent, because there is no order to be had.** The candidates arrive in whatever
/// order the structure hands them over, so they are summed by what they mean rather than by
/// depth: the exact total coverage `1 - prod(1 - a)`, the coverage-weighted colour and the
/// coverage-weighted depth, which is exact for one crossing and for any number of one colour.
///
/// @param limit how far the eye committed. Everything past it is behind a surface and hidden.
/// @param cone the pixel's own cone, so a distant crossing reads its texture at the level that
///        resolves it rather than at the finest one.
Gathered gatherAlong(vec3 origin, vec3 direction, float limit, Cone cone, GatherRule rule)
{
    Gathered gathered;
    gathered.mUnlit = uvec3(0u);
    gathered.mGlowed = uvec3(0u);
    gathered.mCoverage = 0u;
    gathered.mCoveredAt = 0u;
    gathered.mBlocked = 0u;
    gathered.mCoveringNormal = vec3(0.0, 0.0, 1.0);
    gathered.mCoveringAlpha = 0.0;
    gathered.mCoveringAt = limit;
    gathered.mCovering = 0u;

    rayQueryEXT query;
    rayQueryInitializeEXT(query, sceneTop, facingFor(rule.mDraws), rule.mMask, origin, 0.0, direction, limit);

    while (rayQueryProceedEXT(query))
    {
        if (rayQueryGetIntersectionTypeEXT(query, false) != gl_RayQueryCandidateIntersectionTriangleEXT)
            continue;

        const uint instanceIndex = rayQueryGetIntersectionInstanceCustomIndexEXT(query, false);
        const uint primitive = rayQueryGetIntersectionPrimitiveIndexEXT(query, false);
        const vec2 bary = rayQueryGetIntersectionBarycentricsEXT(query, false);
        const float at = rayQueryGetIntersectionTEXT(query, false);

        vec3 corners[3];
        rayQueryGetIntersectionTriangleVertexPositionsEXT(query, false, corners);
        const vec3 crossed = triangleCross(corners, rayQueryGetIntersectionObjectToWorldEXT(query, false));

        const Crossing crossing
            = crossingOf(instanceIndex, primitive, bary, crossed, direction, cone.mWidth + cone.mSpread * at);
        const GpuMaterial material = crossing.mMaterial;
        const vec4 texel = crossing.mTexel;

        // **`sampledOpacity` and not the same arithmetic written again**, so that one surface cannot
        // be hazed two ways: this is the number a shadow ray asks of the same surface. It is handed
        // the alpha rather than the point, because the colour beside it is wanted here and nowhere
        // else. `ONE, ONE` reads no alpha at all; the rest weight what they add by it.
        const bool whole = rule.mWholeAlpha && (material.mFlags & MATERIAL_ADD_WHOLE) != 0u;
        const float painted = whole ? 1.0 : sampledOpacity(surfaceOpacity(crossing.mInstance, material), texel.a);
        if (!(painted > 0.0))
            continue;

        // Degenerate triangles carry no plane, and a crossing with no angle to it is taken square:
        // the plane is then the ray's own, and the dot below is one.
        const float area = length(crossed);
        const vec3 plane = area > 0.0 ? crossed / area : -direction;

        const float alpha = rule.mThickens ? mediumCrossing(painted, abs(dot(plane, direction))) : painted;

        // The vertex colour replaces the tint or the glow where the content asked and the rule
        // reads it, as `resolveFor` reads it, and the same two weights select without a branch.
        // **The material's own glow, summed with the light and not beside it**, which is where the
        // original engine puts it: a surface carrying one glows *with its texture in it*. An
        // emissive *map* is not read — no cloud in the game carries one, and a fetch a crossing
        // for it would be paid by every shell of every one that does not.
        const vec3 vertexColour = rule.mVertexTint ? triangleColour(crossing.mCorner, crossing.mBary) : vec3(1.0);
        const float tinted = rule.mVertexTint ? float((material.mFlags & MATERIAL_VERTEX_TINT) != 0u) : 0.0;
        const float glowing = rule.mVertexTint ? float((material.mFlags & MATERIAL_VERTEX_GLOW) != 0u) : 0.0;

        gathered.mUnlit = addShare(
            gathered.mUnlit, sharePart(texel.rgb * mix(material.mDiffuseColour, vertexColour, tinted) * alpha));
        gathered.mGlowed
            = addShare(gathered.mGlowed, sharePart(mix(material.mEmissiveColour, vertexColour, glowing) * alpha));
        gathered.mCoverage = addShare(gathered.mCoverage, sharePart(alpha));
        gathered.mCoveredAt = addShare(gathered.mCoveredAt, sharePart(at / limit * alpha));
        if (rule.mBlocks)
            gathered.mBlocked = addShare(gathered.mBlocked, blockedBy(alpha));

        // **By what it hid and not by what it was lit by**: an unlit crossing decides the whole of
        // what the pixel shows. **A tie goes to the nearer crossing and then to the lower
        // instance**, because two hiding the same share arrive in the card's order, and the first
        // of them would be the scheduler's choice.
        const bool tied = alpha == gathered.mCoveringAlpha && alpha > 0.0
            && (at < gathered.mCoveringAt || (at == gathered.mCoveringAt && instanceIndex < gathered.mCovering));
        if (alpha > gathered.mCoveringAlpha || tied)
        {
            gathered.mCoveringAlpha = alpha;
            gathered.mCoveringNormal = plane;
            gathered.mCoveringAt = at;
            gathered.mCovering = instanceIndex;
        }
    }

    return gathered;
}

/// Where a gathered layer stands, what lights it there, and what the air in front of it leaves.
///
/// **One light answer for the layer and not one per crossing.** The crossings are gathered first
/// and the one point they came to is lit once, out of the froxel the air's own volume already
/// filled for it — `puffLight` says what that reads and why it is not three rays of the walk's own.
///
/// **The layer taken exactly and the band taken once**, which is what the geometry behind the
/// layer is charged — so a cloud and the mountain behind it fade at one rate. `fogColumn` states
/// the height falloff's integral in closed form, and the band is the term nothing integrates, so
/// it is sampled at the path's mean-value point.
///
/// **The side the layer shows, off the crossing that hid the most of the pixel.** A cloud has a
/// surface where a puff of smoke has only a ball's silhouette, so the wrap that gives a sprite a
/// lit side is read off a real plane here. Turned to face the eye, because a shell is met from
/// either face and which one the winding names carries no meaning on this content. A cloud is
/// smoke, and is lit as a ball of it.
struct GatheredLight
{
    /// How far along the ray the layer stood, coverage-weighted.
    float mSeen;

    /// What the air in front of it leaves.
    float mReaching;

    /// What lights it there, per unit of albedo.
    vec3 mLight;
};

GatheredLight gatheredLight(uvec2 pixel, vec3 origin, vec3 direction, float limit, Gathered gathered)
{
    GatheredLight lit;
    lit.mSeen = float(gathered.mCoveredAt) / float(gathered.mCoverage) * limit;
    lit.mReaching = exp(-fogColumn(origin, direction, lit.mSeen)
        * fogCoverageAt(origin + direction * (0.5 * lit.mSeen), max(lit.mSeen, 1.0)));

    const vec3 normal = faceforward(gathered.mCoveringNormal, direction, gathered.mCoveringNormal);
    lit.mLight = puffLight(pixel, direction, lit.mSeen, ballPuff(normal, smokeThrow(direction)));

    return lit;
}

/// Every medium the eye crosses before `limit`, composited into one layer.
///
/// The shells are thickened by the angle they are crossed at, keep their material's own tint, and
/// report what they let through: what a layer that covers is.
PuffLayer mediumAlong(uvec2 pixel, vec3 origin, vec3 direction, float limit, Cone cone)
{
    PuffLayer layer = noPuffs();

    const Gathered gathered
        = gatherAlong(origin, direction, limit, cone, GatherRule(MASK_MEDIUM, true, false, false, true, false));
    if (gathered.mCoverage == 0u)
        return layer;

    layer.mTransmittance = throughBlocked(gathered.mBlocked);

    // The unit cancels in a ratio of two sums, so neither is scaled back.
    const vec3 albedo = vec3(gathered.mUnlit) / float(gathered.mCoverage);

    const GatheredLight lit = gatheredLight(pixel, origin, direction, limit, gathered);

    // **The glow is not scaled by `EMISSIVE_INTENSITY`**, as a surface's and a sheet's are: the
    // Ghostfence is a medium that glows, and at that scale a night's exposure washes it to white and
    // loses the orbs painted on it.
    layer.mColour = albedo * (lit.mLight + vec3(gathered.mGlowed) / float(gathered.mCoverage)) * lit.mReaching;
    layer.mCoveredAt = lit.mSeen;

    return layer;
}

/// What every additive surface the eye crosses before `limit` adds to the pixel, lit once where
/// they stand — `PuffLayer::mAdded`'s share from the meshes.
///
/// **A magic effect's sheet is a flame with triangles.** The rasterizer draws it `SRC_ALPHA, ONE`
/// over everything, unsorted and undenoised; here it carries `MASK_ADDITIVE` and nothing else, so
/// no shading ray meets it and this one walk gathers it — at the picture's own extent, where
/// `spritecomposite.rgen` marches the flames, and never through the denoiser.
///
/// **Each crossing is what its material states, times its alpha**: the texture's colour under the
/// tint the vertex colour or the material gives it, and the material's glow — which is where the
/// white ambient the game gives an effect landed, `Rtx::SurfaceDescription::mAmbientOverride` — in
/// `EMISSIVE_INTENSITY`, the one statement of what a material's one is worth here. The unlit share
/// is then lit once as a ball of smoke standing at the coverage-weighted depth, as a cloud is, so
/// the ice wall's sheets take their cell's light and an effect's take their own glow.
///
/// @param pixel the traced pixel, which names the froxel column the light is read from.
vec3 additiveAlong(uvec2 pixel, vec3 origin, vec3 direction, float limit, Cone cone)
{
    const Gathered gathered
        = gatherAlong(origin, direction, limit, cone, GatherRule(MASK_ADDITIVE, false, true, true, false, true));
    if (gathered.mCoverage == 0u)
        return vec3(0.0);

    const GatheredLight lit = gatheredLight(pixel, origin, direction, limit, gathered);

    // `mUnlit` already carries every crossing's alpha, so the glow is taken per unit of coverage —
    // a mean over the crossings — and the light likewise: `texel * tint * alpha * (light + glow)`
    // summed over the crossings, which is the rasterizer's own sum.
    return vec3(gathered.mUnlit) / SHARE_UNIT
        * (lit.mLight + vec3(gathered.mGlowed) / float(gathered.mCoverage) * EMISSIVE_INTENSITY) * lit.mReaching;
}

#endif
