// `#pragma once` everywhere else in this tree, and an include guard here for the reason
// `components/rtx/shaders/portable.h` gives.
#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_MEDIUM_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_MEDIUM_GLSL

// The shells of a cloud the content modelled as geometry, gathered along the eye's ray as one
// medium rather than met one surface at a time.
//
// **What this is for.** `meshes/f/active_blight_large.nif` is eleven alpha shells over one another,
// and neither of its textures holds a single opaque texel: 71.8% of `tx_dagoth_cloud.dds` is partly
// there and none of it is solid. A ray tracer that peels the nearest of those and paints the next
// one as though it were opaque draws a cloud as a flat red sheet, which is exactly what it did.
//
// **One traversal for the whole stack.** The eye's own ray walks past every medium it meets —
// `candidateStops` is where that is decided — so it commits the mountain behind the cloud and this
// walk gathers what stands in front of it, on a mask no other instance carries. Re-tracing per layer
// is the alternative: Anagnostou measures it at 9.6 to 12.1 ms in Sponza with an early-out at
// `T < 0.05`, against 1.2 ms for the same picture alpha-blended by a rasterizer. This walk costs
// 0.41 ms of the trace at Dagoth Ur and nothing in a cell with no cloud in it.
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

/// Every medium the eye crosses before `limit`, composited into one layer.
///
/// **One light answer for the layer and not one per shell.** The crossings are gathered first and
/// the one point they came to is lit once, out of the froxel the air's own volume already filled for
/// it — `puffLight` says what that reads and why it is not three rays of the walk's own.
///
/// **The claim it fills is the covering one**, judged by what a crossing hid, exactly as a puff of
/// smoke's is: an unlit shell sends back no light at all and still decides everything the pixel
/// shows. Its travel is the placement's, which the caller fills in — this walk has no reprojection
/// in it and `movedBy` is the frame's to ask.
///
/// @param limit how far the eye committed. Everything past it is behind a surface and hidden.
/// @param cone the pixel's own cone, so a distant shell reads its texture at the level that resolves
///        it rather than at the finest one.
/// @param covering filled with the instance whose crossing hid the most of this pixel, or left alone
///        where nothing did.
PuffLayer mediumAlong(uvec2 pixel, vec3 origin, vec3 direction, float limit, Cone cone, out uint covering)
{
    PuffLayer layer = noPuffs();
    covering = 0u;

    vec3 covered = vec3(0.0);
    vec3 glowed = vec3(0.0);
    float coverage = 0.0;
    float coveredAt = 0.0;

    // The plane of the crossing that hid the most, which is the one side the layer is given.
    vec3 coveringNormal = vec3(0.0, 0.0, 1.0);

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

        const GpuInstance instance = instanceAt(instanceIndex);
        const GpuMaterial material = materialAt(instance.mMaterial);

        vec2 uv[3];
        triangleUvs(triangleCorners(meshAt(instance.mMesh), primitive), uv);
        const TexturePoint point = texturePoint(uv, cornerWeights(bary), material.mTextureTransform);
        const SurfaceCone surfaceCone = surfaceConeAt(crossed, direction);

        const vec4 texel = sampleDiffuse(material.mDiffuse, point, surfaceCone, cone.mWidth + cone.mSpread * at);

        // **`sampledOpacity` and not the same arithmetic written again**, so that one surface cannot
        // be hazed two ways: this is the number a shadow ray asks of the same shell. It is handed
        // the alpha rather than the point, because the colour beside it is wanted here and nowhere
        // else.
        const float painted = sampledOpacity(surfaceOpacity(instance, material), texel.a);
        if (!(painted > 0.0))
            continue;

        // Degenerate triangles carry no plane, and a crossing with no angle to it is taken square.
        const float area = length(crossed);
        const vec3 plane = area > 0.0 ? crossed / area : -direction;

        const float alpha = mediumCrossing(painted, area > 0.0 ? abs(dot(plane, direction)) : 1.0);

        covered += texel.rgb * material.mDiffuseColour * alpha;

        // **The material's own glow, summed with the light and not beside it**, which is where the
        // original engine puts it: a surface carrying one glows *with its texture in it*. An
        // emissive *map* on a medium is not read — no cloud in the game carries one, and a fetch a
        // crossing for it would be paid by every shell of every one that does not.
        glowed += material.mEmissiveColour * alpha;

        coverage += alpha;
        coveredAt += at * alpha;
        layer.mTransmittance *= 1.0 - alpha;

        // **By what it hid and not by what it was lit by**, which is `PuffLayer::mCovering`'s own
        // rule: an unlit shell decides the whole of what the pixel shows.
        if (alpha > layer.mCovering.mWeight)
        {
            layer.mCovering = PuffClaim(direction * at, vec3(0.0), alpha);
            coveringNormal = plane;
            covering = instanceIndex;
        }
    }

    if (!(coverage > 0.0))
        return layer;

    const vec3 albedo = covered / coverage;
    const float seen = coveredAt / coverage;
    const vec3 point = origin + direction * seen;

    // **One evaluation of the fog's field for the whole layer**, taken halfway to it — the
    // mean-value point of the path, and the same economy the sprite walk makes for the same forty
    // hashes a sample costs out of doors.
    const float extinction = fogExtinctionAt(origin + direction * (0.5 * seen), max(seen, 1.0));
    const float reaching = exp(-extinction * seen);

    // **The side the layer shows, off the shell that hid the most of the pixel.** A cloud has a
    // surface where a puff of smoke has only a ball's silhouette, so the wrap that gives a sprite a
    // lit side is read off a real plane here. Turned to face the eye, because a shell is met from
    // either face and which one the winding names carries no meaning on this content.
    const vec3 normal = faceforward(coveringNormal, direction, coveringNormal);

    // A cloud is smoke, and is lit as a ball of it.
    const vec3 light = puffLight(pixel, direction, seen, ballPuff(normal, smokeThrow(direction)));

    layer.mColour = albedo * (light + glowed / coverage) * reaching;
    layer.mCoveredAt = seen;

    return layer;
}

#endif
