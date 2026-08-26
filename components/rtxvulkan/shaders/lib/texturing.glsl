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

/// Which mip a cone this wide should be read from.
///
/// Akenine-Moller's ray-cone formulation: the texel-to-world area ratio of the triangle fixes a
/// base level, the cone's width where it landed moves off it, and the angle the surface presents
/// stretches the footprint when it is seen edge-on. A compute shader has no derivatives, so this is
/// the only thing standing between every fetch and level zero.
/// @param crossed the triangle's edge cross product, whose length is twice its area. The texel area
///        below is doubled the same way, so the two cancel in the ratio.
/// @param coneWidth how wide the ray's cone is where it landed, or zero for a ray that carries no
///        cone at all — which is every shadow ray, and which reads the finest level.
float coneLod(uint slot, vec2 uv0, vec2 uv1, vec2 uv2, vec3 crossed, vec3 direction, float coneWidth)
{
    if (coneWidth <= 0.0)
        return 0.0;

    const float worldArea = length(crossed);
    const float uvArea = abs((uv1.x - uv0.x) * (uv2.y - uv0.y) - (uv2.x - uv0.x) * (uv1.y - uv0.y));
    if (worldArea <= 0.0 || uvArea <= 0.0)
        return 0.0;

    const vec2 size = vec2(textureSize(textures[nonuniformEXT(slot)], 0));
    const float texelArea = uvArea * size.x * size.y;

    // A surface seen edge-on covers more of itself per pixel, and the cone's footprint on it grows
    // by the same factor. Clamped because a grazing hit sends it to infinity.
    const float facing = max(abs(dot(crossed / worldArea, direction)), 1e-3);

    return 0.5 * log2(texelArea / worldArea) + log2(coneWidth) - log2(facing);
}

/// The diffuse texel a hit landed on, read at the level its cone can resolve.
///
/// Shared by everything that asks: the colour of a committed hit, the mask of a candidate one, and
/// each layer of a piece of ground. Sharing it is what keeps them reading the same level — a cutout
/// resolved against a different mip than the surface it cuts would put the hole and the leaf in
/// different places.
/// @param transform mesh texture coordinates to this texture's, as `uv * xy + zw`.
vec4 sampleDiffuse(
    uint slot, vec2 uv[3], vec3 weight, vec4 transform, vec3 crossed, vec3 direction, float coneWidth)
{
    const vec2 uv0 = uv[0] * transform.xy + transform.zw;
    const vec2 uv1 = uv[1] * transform.xy + transform.zw;
    const vec2 uv2 = uv[2] * transform.xy + transform.zw;
    const vec2 at = uv0 * weight.x + uv1 * weight.y + uv2 * weight.z;

    return textureLod(
        textures[nonuniformEXT(slot)], at, coneLod(slot, uv0, uv1, uv2, crossed, direction, coneWidth));
}

/// The light a texture already carries at `at`, bilinear across its grid and wrapping with it.
///
/// Wrapping because Morrowind's textures tile and a great many of them rely on it: a map that
/// clamped at the edges would put a seam down every wall that repeats.
float paintedLight(uint slot, vec2 at)
{
    const vec2 grid = fract(at) * float(SHADING_EXTENT) - 0.5;
    const ivec2 low = ivec2(floor(grid));
    const vec2 across = grid - vec2(low);

    const ivec2 first = (low % int(SHADING_EXTENT) + int(SHADING_EXTENT)) % int(SHADING_EXTENT);
    const ivec2 second = (first + 1) % int(SHADING_EXTENT);
    const uint base = slot * SHADING_EXTENT * SHADING_EXTENT;

    const float topLeft = shading[base + uint(first.y) * SHADING_EXTENT + uint(first.x)];
    const float topRight = shading[base + uint(first.y) * SHADING_EXTENT + uint(second.x)];
    const float bottomLeft = shading[base + uint(second.y) * SHADING_EXTENT + uint(first.x)];
    const float bottomRight = shading[base + uint(second.y) * SHADING_EXTENT + uint(second.x)];

    return mix(mix(topLeft, topRight, across.x), mix(bottomLeft, bottomRight, across.x), across.y);
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
vec3 sampleAlbedo(uint slot, vec2 uv[3], vec3 weight, vec4 transform, vec3 crossed, vec3 direction, float coneWidth)
{
    const vec3 texel = sampleDiffuse(slot, uv, weight, transform, crossed, direction, coneWidth).rgb;
    if (frame.mDelight <= 0.0)
        return texel;

    // The same point `sampleDiffuse` read, worked out again rather than handed back: it is six
    // multiplies against an out-parameter on the hottest sampler in the shader, and it is not
    // computed at all when nothing is being divided out.
    const vec2 uv0 = uv[0] * transform.xy + transform.zw;
    const vec2 uv1 = uv[1] * transform.xy + transform.zw;
    const vec2 uv2 = uv[2] * transform.xy + transform.zw;
    const vec2 at = uv0 * weight.x + uv1 * weight.y + uv2 * weight.z;

    return texel / mix(1.0, paintedLight(slot, at), frame.mDelight);
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

    return mix(mix(masks[row0 + uint(base.x)], masks[row0 + uint(high.x)], frac.x),
        mix(masks[row1 + uint(base.x)], masks[row1 + uint(high.x)], frac.x), frac.y);
}

#endif
