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

/// One crossing of a walk that confirms nothing, read down to what a walk weighs it by: the rows
/// the candidate names, the corner the texel is read at, and the texel. The medium walk and the
/// additive walk read a crossing alike and weigh it differently, so the reading is said once.
///
/// @param coneWidth how wide the ray's cone is at the crossing, which picks the texel's level.
struct Crossing
{
    GpuInstance mInstance;
    GpuMaterial mMaterial;
    uvec3 mCorner;
    vec3 mWeight;
    vec4 mTexel;
};

Crossing crossingOf(uint instanceIndex, uint primitive, vec2 bary, vec3 crossed, vec3 direction, float coneWidth)
{
    Crossing crossing;
    crossing.mInstance = instanceAt(instanceIndex);
    crossing.mMaterial = materialAt(crossing.mInstance.mMaterial);
    crossing.mCorner = triangleCorners(meshAt(crossing.mInstance.mMesh), primitive);
    crossing.mWeight = cornerWeights(bary);

    vec2 uv[3];
    triangleUvs(crossing.mCorner, uv);
    const TexturePoint point = texturePoint(
        uv, crossing.mWeight, crossing.mMaterial.mTextureTransform, surfaceConeAt(crossed, direction), coneWidth);

    crossing.mTexel = crossing.mMaterial.mDiffuse == NO_TEXTURE ? vec4(NO_TEXTURE_ALBEDO, 1.0)
                                                                : sampleDiffuse(crossing.mMaterial.mDiffuse, point);
    return crossing;
}

/// Every medium the eye crosses before `limit`, composited into one layer.
///
/// **One light answer for the layer and not one per shell.** The crossings are gathered first and
/// the one point they came to is lit once, out of the froxel the air's own volume already filled for
/// it — `puffLight` says what that reads and why it is not three rays of the walk's own.
///
/// @param limit how far the eye committed. Everything past it is behind a surface and hidden.
/// @param cone the pixel's own cone, so a distant shell reads its texture at the level that resolves
///        it rather than at the finest one.
PuffLayer mediumAlong(uvec2 pixel, vec3 origin, vec3 direction, float limit, Cone cone)
{
    PuffLayer layer = noPuffs();

    // **In `sharePart`'s units, because the shells arrive in the card's order** — `SHARE_UNIT` says
    // what a float sum taken in that order did to the frame hash. The distance is summed as a share
    // of `limit`, which is the one bound a crossing has.
    uvec3 covered = uvec3(0u);
    uvec3 glowed = uvec3(0u);
    uint coverage = 0u;
    uint coveredAt = 0u;
    uint blocked = 0u;

    // The plane of the crossing that hid the most, which is the one side the layer is given, how
    // much it hid, how far off it was and which instance it was.
    vec3 coveringNormal = vec3(0.0, 0.0, 1.0);
    float coveringAlpha = 0.0;
    float coveringAt = limit;
    uint covering = 0u;

    rayQueryEXT query;
    rayQueryInitializeEXT(query, sceneTop, gl_RayFlagsNoneEXT, MASK_MEDIUM, origin, 0.0, direction, limit);

    while (rayQueryProceedEXT(query))
    {
        if (rayQueryGetIntersectionTypeEXT(query, false) != gl_RayQueryCandidateIntersectionTriangleEXT)
            continue;

        // **Nothing is ever confirmed here.** A medium is not a surface, so the walk runs to `limit`
        // and every crossing on the way is kept; the loop ends because the candidates do.
        const uint instanceIndex = rayQueryGetIntersectionInstanceCustomIndexEXT(query, false);
        const uint primitive = rayQueryGetIntersectionPrimitiveIndexEXT(query, false);
        const vec2 bary = rayQueryGetIntersectionBarycentricsEXT(query, false);
        const float at = rayQueryGetIntersectionTEXT(query, false);

        vec3 corners[3];
        rayQueryGetIntersectionTriangleVertexPositionsEXT(query, false, corners);
        const vec3 crossed = triangleCross(corners, rayQueryGetIntersectionObjectToWorldEXT(query, false));

        const Crossing shell
            = crossingOf(instanceIndex, primitive, bary, crossed, direction, cone.mWidth + cone.mSpread * at);
        const GpuMaterial material = shell.mMaterial;
        const vec4 texel = shell.mTexel;

        // **`sampledOpacity` and not the same arithmetic written again**, so that one surface cannot
        // be hazed two ways: this is the number a shadow ray asks of the same shell. It is handed
        // the alpha rather than the point, because the colour beside it is wanted here and nowhere
        // else.
        const float painted = sampledOpacity(surfaceOpacity(shell.mInstance, material), texel.a);
        if (!(painted > 0.0))
            continue;

        // Degenerate triangles carry no plane, and a crossing with no angle to it is taken square.
        const float area = length(crossed);
        const vec3 plane = area > 0.0 ? crossed / area : -direction;

        const float alpha = mediumCrossing(painted, area > 0.0 ? abs(dot(plane, direction)) : 1.0);

        covered = addShare(covered, sharePart(texel.rgb * material.mDiffuseColour * alpha));

        // **The material's own glow, summed with the light and not beside it**, which is where the
        // original engine puts it: a surface carrying one glows *with its texture in it*. An
        // emissive *map* on a medium is not read — no cloud in the game carries one, and a fetch a
        // crossing for it would be paid by every shell of every one that does not.
        glowed = addShare(glowed, sharePart(material.mEmissiveColour * alpha));

        coverage = addShare(coverage, sharePart(alpha));
        coveredAt = addShare(coveredAt, sharePart(at / limit * alpha));
        blocked = addShare(blocked, blockedBy(alpha));

        // **By what it hid and not by what it was lit by**: an unlit shell decides the whole of
        // what the pixel shows. **A tie goes to the nearer shell and then to the lower instance**,
        // because two shells hiding the same share arrive in the card's order, and the first of
        // them would be the scheduler's choice.
        const bool tied
            = alpha == coveringAlpha && alpha > 0.0 && (at < coveringAt || (at == coveringAt && instanceIndex < covering));
        if (alpha > coveringAlpha || tied)
        {
            coveringAlpha = alpha;
            coveringNormal = plane;
            coveringAt = at;
            covering = instanceIndex;
        }
    }

    if (coverage == 0u)
        return layer;

    layer.mTransmittance = throughBlocked(blocked);

    // The unit cancels in a ratio of two sums, so neither is scaled back.
    const vec3 albedo = vec3(covered) / float(coverage);
    const float seen = float(coveredAt) / float(coverage) * limit;

    // **The layer taken exactly and the band taken once**, which is what the geometry behind this
    // shell is charged — so a cloud and the mountain behind it fade at one rate. `fogColumn` states
    // the height falloff's integral in closed form, and the band is the term nothing integrates, so
    // it is sampled at the path's mean-value point.
    const float reaching
        = exp(-fogColumn(origin, direction, seen) * fogCoverageAt(origin + direction * (0.5 * seen), max(seen, 1.0)));

    // **The side the layer shows, off the shell that hid the most of the pixel.** A cloud has a
    // surface where a puff of smoke has only a ball's silhouette, so the wrap that gives a sprite a
    // lit side is read off a real plane here. Turned to face the eye, because a shell is met from
    // either face and which one the winding names carries no meaning on this content.
    const vec3 normal = faceforward(coveringNormal, direction, coveringNormal);

    // A cloud is smoke, and is lit as a ball of it.
    const vec3 light = puffLight(pixel, direction, seen, ballPuff(normal, smokeThrow(direction)));

    layer.mColour = albedo * (light + vec3(glowed) / float(coverage)) * reaching;
    layer.mCoveredAt = seen;

    return layer;
}

/// What every additive surface the eye crosses before `limit` adds to the pixel, lit once where
/// they stand — `PuffLayer::mAdded`'s share from the meshes.
///
/// **A magic effect's sheet is a flame with triangles.** The rasterizer draws it `SRC_ALPHA, ONE`
/// over everything, unsorted and undenoised; here it carries `MASK_ADDITIVE` and nothing else, so
/// no shading ray meets it and this one query gathers it — at the picture's own extent, where
/// `spritecomposite.rgen` marches the flames, and never through the denoiser. Nothing is ever
/// confirmed: what adds covers nothing, so the walk runs to `limit` and keeps every crossing.
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
    // In `sharePart`'s units, for the reason `mediumAlong` gives: the crossings arrive in the
    // card's order.
    uvec3 unlit = uvec3(0u);
    uvec3 glowed = uvec3(0u);
    uint coverage = 0u;
    uint coveredAt = 0u;

    vec3 coveringNormal = vec3(0.0, 0.0, 1.0);
    float coveringAlpha = 0.0;
    float coveringAt = limit;
    uint covering = 0u;

    rayQueryEXT query;
    rayQueryInitializeEXT(query, sceneTop, gl_RayFlagsNoneEXT, MASK_ADDITIVE, origin, 0.0, direction, limit);

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

        const Crossing sheet
            = crossingOf(instanceIndex, primitive, bary, crossed, direction, cone.mWidth + cone.mSpread * at);
        const GpuMaterial material = sheet.mMaterial;
        const vec4 texel = sheet.mTexel;

        // `ONE, ONE` reads no alpha at all; the rest weight what they add by it, as a flame does.
        const bool whole = (material.mFlags & MATERIAL_ADD_WHOLE) != 0u;
        const float alpha = whole ? 1.0 : sampledOpacity(surfaceOpacity(sheet.mInstance, material), texel.a);
        if (!(alpha > 0.0))
            continue;

        // The vertex colour replaces the tint or the glow where the content asked, as `resolveFor`
        // reads it, and the same two weights select without a branch.
        const vec3 vertexColour = triangleColour(sheet.mCorner, sheet.mWeight);
        const float tinted = float((material.mFlags & MATERIAL_VERTEX_TINT) != 0u);
        const float glowing = float((material.mFlags & MATERIAL_VERTEX_GLOW) != 0u);

        unlit = addShare(unlit, sharePart(texel.rgb * mix(material.mDiffuseColour, vertexColour, tinted) * alpha));
        glowed = addShare(glowed, sharePart(mix(material.mEmissiveColour, vertexColour, glowing) * alpha));
        coverage = addShare(coverage, sharePart(alpha));
        coveredAt = addShare(coveredAt, sharePart(at / limit * alpha));

        const float area = length(crossed);
        const bool tied
            = alpha == coveringAlpha && alpha > 0.0 && (at < coveringAt || (at == coveringAt && instanceIndex < covering));
        if (alpha > coveringAlpha || tied)
        {
            coveringAlpha = alpha;
            coveringNormal = area > 0.0 ? crossed / area : -direction;
            coveringAt = at;
            covering = instanceIndex;
        }
    }

    if (coverage == 0u)
        return vec3(0.0);

    const float seen = float(coveredAt) / float(coverage) * limit;

    // The air in front of the layer, taken once at the layer's own depth, as `mediumAlong` takes
    // it: what a sheet adds is thinned by the fog short of it like anything else that stands there.
    const float reaching
        = exp(-fogColumn(origin, direction, seen) * fogCoverageAt(origin + direction * (0.5 * seen), max(seen, 1.0)));

    const vec3 normal = faceforward(coveringNormal, direction, coveringNormal);
    const vec3 light = puffLight(pixel, direction, seen, ballPuff(normal, smokeThrow(direction)));

    // `unlit` already carries every crossing's alpha, so the glow is taken per unit of coverage —
    // a mean over the crossings — and the light likewise: `texel * tint * alpha * (light + glow)`
    // summed over the crossings, which is the rasterizer's own sum.
    return vec3(unlit) / SHARE_UNIT * (light + vec3(glowed) / float(coverage) * EMISSIVE_INTENSITY) * reaching;
}

#endif
