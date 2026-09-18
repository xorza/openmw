#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_GROUND_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_GROUND_GLSL

// A piece of ground, as one texel of it is read: which of its layers show there, and what each
// shows, with the light painted into the layer's texture divided back out.
//
// **Its own file because two shaders read the ground and neither may read it differently.** The
// trace sums a chunk's stack at every hit near enough to keep it; `groundcomposite.comp` sums
// the same stack once into one texture for a chunk far enough to flatten. A flattened chunk and
// its live stack have to be the same ground, so the sum is written here, over the tables handed
// in by address rather than reached through the trace's frame block, which the bake has not got.
//
// **The three tables it reads are declared here** and reached through `bindings.glsl` by the
// trace, so that a table is declared once whichever side constructs the reference.

#include "look.h"
#include "scene.h"

#include "texturearray.glsl"

layout(buffer_reference, scalar, buffer_reference_align = TABLE_ALIGN_ROWS) readonly buffer MaterialTable
{
    GpuMaterial at[];
};

layout(buffer_reference, scalar, buffer_reference_align = TABLE_ALIGN_LAYERS) readonly buffer LayerTable
{
    GpuLayer at[];
};

layout(buffer_reference, scalar, buffer_reference_align = TABLE_ALIGN_ROWS) readonly buffer MaskTable
{
    float at[];
};

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

/// A texture's albedo at `at`, read at `lod`, with the light painted into it divided back out by
/// `delight` of the estimate.
///
/// **A texture drawn for a renderer with no bounce has the bounce drawn into it** — occlusion in
/// the corners, a highlight along a rim, the glow a lamp throws on the wall behind it. Lighting it
/// again puts every one of those in twice, so what is wanted from the file is the colour underneath
/// and the estimate is what takes the rest off.
///
/// Only where an albedo is being read. The same sampler serves a cutout's mask, which is alpha and
/// unaffected, and an emissive map, which is light rather than a surface and must keep what it was
/// painted with.
vec3 sampleAlbedoLod(uint slot, vec2 at, float lod, float delight)
{
    const vec3 texel = textureLod(textures[nonuniformEXT(slot)], at, lod).rgb;
    if (delight <= 0.0)
        return texel;

    return texel / mix(1.0, paintedLight(slot, at), delight);
}

/// Where `chunkUv` of a chunk lands on one of its layers, which tiles across it.
vec2 layerUv(GpuLayer layer, vec2 chunkUv)
{
    return chunkUv * layer.mDiffuseTransform.xy + layer.mDiffuseTransform.zw;
}

/// How much of a terrain layer shows at `uv` of its chunk, from its grid of weights in `masks`.
///
/// Sampled by hand rather than through a sampler because the grid is ten texels across and lives in
/// a buffer, and because a mask has to clamp at its edges — the one sampler every texture in this
/// scene shares repeats, which is what the tiling ground needs and the mask cannot have.
float maskWeightIn(GpuLayer layer, vec2 uv, MaskTable masks)
{
    // A chunk of one ground type is given no mask at all: there is nothing to blend it against.
    if (layer.mMaskWidth == 0u || layer.mMaskHeight == 0u)
        return 1.0;

    const ivec2 grid = ivec2(layer.mMaskWidth, layer.mMaskHeight);

    // Held inside the mask, because a mask clamps at its edges where every other texture repeats:
    // a transform that carried the point past one would read past the run.
    const vec2 at = clamp(uv * layer.mMaskTransform.xy + layer.mMaskTransform.zw, 0.0, 1.0);

    // Texel centres sit at half-integers, so the bilinear footprint starts half a texel back.
    const vec2 texel = at * vec2(grid) - 0.5;
    const vec2 frac = fract(texel);
    const ivec2 low = ivec2(floor(texel));
    const ivec2 high = min(low + 1, grid - 1);
    const ivec2 base = max(low, ivec2(0));

    const uint row0 = layer.mMaskOffset + uint(base.y) * layer.mMaskWidth;
    const uint row1 = layer.mMaskOffset + uint(high.y) * layer.mMaskWidth;

    return mix(mix(masks.at[row0 + uint(base.x)], masks.at[row0 + uint(high.x)], frac.x),
        mix(masks.at[row1 + uint(base.x)], masks.at[row1 + uint(high.x)], frac.x), frac.y);
}

/// The level a layer is read at when a chunk of it is flattened `extent` texels across: how
/// many texels of the layer cross the chunk along its wider axis, against the texels the
/// composite spends on it, as a power of two — a bake has no cone, and this is the footprint one
/// would resolve. Held to the levels the texture has, and never finer than nought.
float groundLod(uint slot, GpuLayer layer, uint extent)
{
    const vec2 size = vec2(textureSize(textures[nonuniformEXT(slot)], 0));
    const vec2 across = size * abs(layer.mDiffuseTransform.xy);
    const float footprint = max(max(across.x, across.y), float(extent)) / float(extent);
    const float deepest = float(textureQueryLevels(textures[nonuniformEXT(slot)]) - 1);

    return clamp(log2(footprint), 0.0, deepest);
}

#endif
