// `#pragma once` everywhere else in this tree, and an include guard here for the reason
// `components/rtx/shaders/portable.h` gives.
#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_TEXTURING_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_TEXTURING_GLSL

// Reading a texture at the level the ray's cone can resolve, and taking the painted-in
// lighting back out of it.
//
// Shared by everything that samples — a committed hit's colour, a candidate hit's cutout
// mask, and each layer of a piece of ground — which is what keeps them reading one level.

#include "scene.h"
#include "bindings.glsl"

/// What the hit's own triangle contributes to a mip level, before any texture is named.
///
/// **One statement of the half of `coneBase` that every point on a hit's triangle shares.** A
/// terrain chunk builds four or five points there, one a layer; each needs the same square root,
/// the same divide by it and the same dot against the ray. The triangle and the ray are one pair
/// per hit, so this is worked out once and handed over.
///
/// **It measures at no time either way**, because `glslc` inlines these and removes some of the
/// repeat itself. A fact every point on the triangle needs is stated once regardless, and stating
/// it does not depend on the optimizer going on making that choice.
struct SurfaceCone
{
    /// Twice the triangle's area in the world. Nought for a degenerate one, which reads level zero.
    float mArea;

    /// A surface seen edge-on covers more of itself per pixel, and the cone's footprint on it grows
    /// by the same factor. Floored, because a grazing hit sends it to infinity.
    float mFacing;
};

/// @param crossed the triangle's edge cross product, whose length is twice its area. The texel area
///        in `coneLod` is doubled the same way, so the two cancel in the ratio.
SurfaceCone surfaceConeAt(vec3 crossed, vec3 direction)
{
    const float area = length(crossed);
    if (!(area > 0.0))
        return SurfaceCone(0.0, 1.0);

    return SurfaceCone(area, max(abs(dot(crossed / area, direction)), 1e-3));
}

/// A base at or below which every texture reads its finest level, whatever its own resolution adds.
///
/// **Both a sentinel and a threshold, and nothing has to know which a given point is.** A ray with
/// no cone is given this so that `coneLod` reads no texture header for it, and a base that reaches
/// it honestly would clamp to the finest level anyway — the largest `0.5 * log2(w * h)` a
/// `maxImageDimension2D` of 16384 allows is 14.
const float TEXTURE_FINEST_BASE = -64.0;

/// Every term of the level but the texture's own resolution: the texel-to-world area ratio of the
/// triangle, the cone's width where it landed, and the angle the surface presents.
///
/// @param coneWidth how wide the ray's cone is where it landed, or zero for a ray that carries no
///        cone at all — which is every shadow ray, and which reads the finest level.
float coneBase(vec2 uv0, vec2 uv1, vec2 uv2, SurfaceCone cone, float coneWidth)
{
    if (!(coneWidth > 0.0) || !(cone.mArea > 0.0))
        return TEXTURE_FINEST_BASE;

    const float uvArea = abs((uv1.x - uv0.x) * (uv2.y - uv0.y) - (uv2.x - uv0.x) * (uv1.y - uv0.y));
    if (!(uvArea > 0.0))
        return TEXTURE_FINEST_BASE;

    return 0.5 * log2(uvArea / cone.mArea) + log2(coneWidth) - log2(cone.mFacing);
}

/// Where on a texture a hit lands, and how coarse a level the ray's cone can still tell apart
/// there.
///
/// **One statement of a hit's place on a sheet, for every read made of that sheet.** A surface reads
/// its albedo, its opacity and its emissive map off one transform, so the three transformed corners
/// are worked out once per transform and handed to every read.
struct TexturePoint
{
    /// The hit, in the texture's own coordinates.
    vec2 mAt;

    /// The level any texture on this sheet is read at, before its own resolution is added.
    ///
    /// **The answer and not what it was made from.** The three transformed corners travelled to
    /// every sampler beside the surface's own cone so that each could take the same determinant
    /// again — eight floats through the call chain for one scalar that is the same for every map on
    /// the triangle, and a surface reads three of them.
    float mBase;
};

/// @param transform mesh texture coordinates to this texture's, as `uv * xy + zw`.
TexturePoint texturePoint(vec2 uv[3], vec3 weight, vec4 transform, SurfaceCone cone, float coneWidth)
{
    const vec2 corner0 = uv[0] * transform.xy + transform.zw;
    const vec2 corner1 = uv[1] * transform.xy + transform.zw;
    const vec2 corner2 = uv[2] * transform.xy + transform.zw;

    return TexturePoint(corner0 * weight.x + corner1 * weight.y + corner2 * weight.z,
        coneBase(corner0, corner1, corner2, cone, coneWidth));
}

/// Which mip one texture on that sheet should be read from.
///
/// Akenine-Moller's ray-cone formulation, split where JCGT 10(1) 2021 section 6 splits it: one term
/// in the texture's own resolution and one term in nothing else. A compute shader has no
/// derivatives, so this is the only thing standing between every fetch and level zero.
///
/// **The early answer is a read and not an arithmetic saving.** `lightThrough` says why a shadow
/// ray takes level zero, and what it saves is the texture header this reads.
float coneLod(uint slot, TexturePoint point)
{
    if (point.mBase <= TEXTURE_FINEST_BASE)
        return 0.0;

    const vec2 size = vec2(textureSize(textures[nonuniformEXT(slot)], 0));

    return point.mBase + 0.5 * log2(size.x * size.y);
}

/// The diffuse texel a hit landed on, read at the level its cone can resolve.
///
/// Shared by everything that asks: the colour of a committed hit, the mask of a candidate one, and
/// each layer of a piece of ground. Sharing it is what keeps them reading the same level — a cutout
/// resolved against a different mip than the surface it cuts would put the hole and the leaf in
/// different places.
vec4 sampleDiffuse(uint slot, TexturePoint point)
{
    return textureLod(textures[nonuniformEXT(slot)], point.mAt, coneLod(slot, point));
}

/// The light a texture already carries at `at`, bilinear across its grid and wrapping with it.
///
/// **One fetch through the array's own sampler, which does the wrap and the blend.** Read out of a
/// buffer by hand this was four loads and the modulo apiece on every albedo read of every hit and
/// every ground layer; measured, the loads cost nothing the trace can see, and the fetch is here for
/// what it is rather than for what it saves. Wrapping because Morrowind's textures tile and a great
/// many of them rely on it: a map that clamped at the edges would put a seam down every wall that
/// repeats.
///
/// Decoded after the filter, which is exact: a blend of stored values decodes to the same blend of
/// the values they stand for, because the decode is affine.
float paintedLight(uint slot, vec2 at)
{
    return mix(SHADING_FLOOR, SHADING_CEILING, textureLod(shadingMaps[nonuniformEXT(slot)], at, 0.0).r);
}

/// The albedo a hit landed on, with the light painted into the texture divided back out.
///
/// **A texture drawn for a renderer with no bounce has the bounce drawn into it** — occlusion in
/// the corners, a highlight along a rim, the glow a lamp throws on the wall behind it. Lighting it
/// again puts every one of those in twice, so what is wanted from the file is the colour underneath
/// and the estimate is what takes the rest off.
///
/// Only where an albedo is being read. The same sampler serves a cutout's mask, which is alpha and
/// unaffected, and an emissive map, which is light rather than a surface and must keep what it was
/// painted with.
vec3 sampleAlbedo(uint slot, TexturePoint point)
{
    const vec3 texel = sampleDiffuse(slot, point).rgb;
    if (frame.mDelight <= 0.0)
        return texel;

    return texel / mix(1.0, paintedLight(slot, point.mAt), frame.mDelight);
}

/// How much of a terrain layer shows at `uv`, from its grid of weights.
///
/// Sampled by hand rather than through a sampler because the grid is ten texels across and lives in
/// a buffer, and because a mask has to clamp at its edges — the one sampler every texture in this
/// scene shares repeats, which is what the tiling ground needs and the mask cannot have.
float maskWeight(GpuLayer layer, vec2 uv)
{
    // A chunk of one ground type is given no mask at all: there is nothing to blend it against.
    if (layer.mMaskWidth == 0u || layer.mMaskHeight == 0u)
        return 1.0;

    const ivec2 grid = ivec2(layer.mMaskWidth, layer.mMaskHeight);
    const vec2 at = uv * layer.mMaskTransform.xy + layer.mMaskTransform.zw;

    // Texel centres sit at half-integers, so the bilinear footprint starts half a texel back.
    const vec2 texel = at * vec2(grid) - 0.5;
    const vec2 frac = fract(texel);
    const ivec2 low = ivec2(floor(texel));
    const ivec2 high = min(low + 1, grid - 1);
    const ivec2 base = max(low, ivec2(0));

    const uint row0 = layer.mMaskOffset + uint(base.y) * layer.mMaskWidth;
    const uint row1 = layer.mMaskOffset + uint(high.y) * layer.mMaskWidth;

    return mix(mix(maskAt(row0 + uint(base.x)), maskAt(row0 + uint(high.x)), frac.x),
        mix(maskAt(row1 + uint(base.x)), maskAt(row1 + uint(high.x)), frac.x), frac.y);
}

#endif
