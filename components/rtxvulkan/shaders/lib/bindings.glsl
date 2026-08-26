// `#pragma once` everywhere else in this tree, and an include guard here for the reason
// `components/rtx/shaders/portable.h` gives.
#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_BINDINGS_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_BINDINGS_GLSL

// Everything the trace is handed, and the three accessors that resolve a global vertex
// or index id to the block it lives in.
//
// **One file, because a binding number is a fact shared with `VisibilityPass` and nothing
// else here has an opinion about it.** What each channel is *for* is written beside it.

#include "gbuffer.h"
#include "scene.h"
#include "visibility.h"

layout(set = 0, binding = 0) uniform accelerationStructureEXT sceneTop;

/// Everything already resolved: direct light, emission, the sky, water, and the fog over all of it.
layout(set = 0, binding = 1, GBUFFER_RADIANCE) uniform writeonly image2D direct;

// One atomic per hit on a single address, which looks like contention and measures as nothing: at
// 3840x2160 over Seyda Neen the trace runs 0.57-0.79 ms, and a subgroup reduction in place of this
// ran 0.65-0.78 for an identical count. Only 5.5% of rays hit, and the reduction would have cost the
// device a subgroup-arithmetic requirement it does not otherwise need. Measure again if a pass ever
// hits most of its pixels.
layout(set = 0, binding = 2) buffer HitCount
{
    uint hits;
};

// The vertex attributes and the indices, as lists of blocks.
//
// **A block is allocated once at its full size and never moved**, so a scene that grows keeps every
// address already handed out and every acceleration structure built from one stays valid — which is
// what lets a cell arriving append rather than rebuild the world. What is bound here is *where* the
// blocks are; a global id resolves to one of them and an offset inside it. `Rtx::SceneDesc` never
// lets a mesh's run straddle a block, and both sizes are powers of two, so that is a shift and a
// mask.
//
// The alignment is four: a twelve-byte element at an arbitrary index is only ever float-aligned.
layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer NormalBlock
{
    vec3 at[];
};

layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer TexCoordBlock
{
    vec2 at[];
};

layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer IndexBlock
{
    uint at[];
};

layout(set = 0, binding = 3, scalar) readonly buffer NormalBlocks
{
    uint64_t normalBlocks[];
};

layout(set = 0, binding = 4, scalar) readonly buffer TexCoordBlocks
{
    uint64_t texCoordBlocks[];
};

layout(set = 0, binding = 5, scalar) readonly buffer IndexBlocks
{
    uint64_t indexBlocks[];
};

vec3 normalAt(uint vertex)
{
    return NormalBlock(normalBlocks[vertex / VERTEX_BLOCK]).at[vertex % VERTEX_BLOCK];
}

vec2 texCoordAt(uint vertex)
{
    return TexCoordBlock(texCoordBlocks[vertex / VERTEX_BLOCK]).at[vertex % VERTEX_BLOCK];
}

uint indexAt(uint element)
{
    return IndexBlock(indexBlocks[element / INDEX_BLOCK]).at[element % INDEX_BLOCK];
}

layout(set = 0, binding = 6, scalar) readonly buffer Meshes
{
    GpuMesh meshes[];
};

layout(set = 0, binding = 7, scalar) readonly buffer Instances
{
    GpuInstance instances[];
};

layout(set = 0, binding = 8, scalar) readonly buffer Materials
{
    GpuMaterial materials[];
};

layout(set = 0, binding = 9, scalar) readonly buffer Layers
{
    GpuLayer layers[];
};

layout(set = 0, binding = 10, scalar) readonly buffer Masks
{
    float masks[];
};

layout(set = 0, binding = 11, scalar) readonly buffer Lights
{
    GpuLight lights[];
};

/// Where each cell's lamps start, with a sentinel so the last cell's end needs no special case.
layout(set = 0, binding = 12, scalar) readonly buffer LightOffsets
{
    uint lightOffsets[];
};

/// Every cell's lamps, run together in cell order.
layout(set = 0, binding = 13, scalar) readonly buffer LightIndices
{
    uint lightIndices[];
};

layout(set = 0, binding = 14, scalar) readonly buffer Waves
{
    GpuWave waves[];
};

/// The blue-noise tile, `RANDOM_STREAMS` channels interleaved per pixel. See `Rtx::BlueNoise`.
layout(set = 0, binding = 16, scalar) readonly buffer BlueNoiseTile
{
    float blueNoise[];
};

/// Clip depth in `r`, for whatever upscales the frame, and the distance from the eye in `g`, for
/// whatever filters it. Two questions, and one number cannot answer both.
layout(set = 0, binding = 22, GBUFFER_DEPTH) uniform writeonly image2D depth;

/// Where the lamps were binned, which is scene geometry rather than camera geometry.
layout(set = 0, binding = 21, scalar) readonly buffer LightGridBlock
{
    GpuLightGrid grid;
};

/// Where each surface stood on the previous frame's screen, less where it stands on this one.
layout(set = 0, binding = 20, GBUFFER_MOTION) uniform writeonly image2D motion;

/// What each texture already has painted into it, `SHADING_EXTENT` squared factors apiece and one
/// texture after another. A texture with no estimate holds ones.
layout(set = 0, binding = 19, scalar) readonly buffer ShadingMaps
{
    float shading[];
};

/// One bounce with the albedo divided out, times whatever the path took off it on the way to the
/// eye — the only channel a filter is allowed to touch.
///
/// **Demodulated because a blur must not touch texture.** What varies slowly across a wall is the
/// light landing on it; what varies fast is the wall. Dividing the albedo out leaves only the first,
/// and the composite multiplies the second back in at full sharpness.
///
/// **And the water and the air ride here rather than with the albedo.** Both are `colour * a + b`
/// over everything in front of the eye, so applying them to a sum applies them to each term: `b`
/// goes into `direct` and `a` belongs to whichever term it attenuated, which is this one. Putting
/// it on the albedo instead made that channel a product of a surface and a path, and an upscaler
/// asking what the surface is got the weather in the answer.
layout(set = 0, binding = 15, GBUFFER_RADIANCE) uniform writeonly image2D indirect;

/// The surface's own diffuse albedo, and nothing else.
///
/// What the composite multiplies the bounce back in by, and what Ray Reconstruction demodulates the
/// diffuse half of a pixel by. Zero where there is no diffuse response at all — the sky, and the
/// water, which answers a ray with a reflection and a refraction and no Lambert term.
layout(set = 0, binding = 17, GBUFFER_ALBEDO) uniform writeonly image2D albedo;

/// The shading normal in `xyz` and the surface's roughness in `w`.
///
/// **The normal the shading actually used**, which for water is the wave's rather than the plane's
/// — a rippled surface described as a flat one is reconstructed as a flat one. A ray that hit
/// nothing writes a zero normal, which no surface can be mistaken for.
layout(set = 0, binding = 18, GBUFFER_GUIDE) uniform writeonly image2D guide;

/// The specular albedo, which is what an upscaler demodulates the mirrored half of a pixel by.
///
/// **Zero wherever the shading was Lambert, which is every solid surface this renderer has.** That
/// is a statement about the shading model and not a placeholder: nothing here answers a ray with a
/// specular lobe except the water, so nothing else has a specular albedo to report. Half floats,
/// because an albedo is a fraction that is never accumulated — the argument for full floats on the
/// radiance channels does not reach here.
layout(set = 0, binding = 26, GBUFFER_ALBEDO) uniform writeonly image2D specular;

/// Where a sprite reached, as one or nought.
///
/// **The one thing in the frame that carries no motion of its own.** One motion vector is written
/// per pixel, from the surface a primary ray hit, so every emitter — rain, snow, ash, smoke — is
/// reprojected with whatever geometry stands behind it. This is what tells the upscaler which pixels
/// those are, and it is free: the composite below already knows what the sprites left.
layout(set = 0, binding = 27, GBUFFER_MASK) uniform writeonly image2D particleMask;

/// Where the past is not worth carrying forward, from nought to one.
///
/// The sprites above, and the water with them, for the reason `GBuffer::getBiasMask` gives.
layout(set = 0, binding = 28, GBUFFER_MASK) uniform writeonly image2D biasMask;

/// Where what the water reflects stood on the previous frame's screen, in pixels. Nought everywhere
/// that is not water reflecting a surface.
layout(set = 0, binding = 29, GBUFFER_MOTION) uniform writeonly image2D reflectionMotion;

/// Every live particle in the scene, one emitter's run after another's.
layout(set = 0, binding = 23, scalar) readonly buffer Sprites
{
    GpuSprite sprites[];
};

/// One sphere and one run of sprites per particle system, indexed by `GpuSprite::mEmitter`.
///
/// **No count beside it, because nothing walks these.** The buffer never shrinks, so its length
/// outlives the cell that filled it — and since the sprite tiles replaced the loop over emitters,
/// the only way in is from a sprite the tile named, which can only name one that is real.
layout(set = 0, binding = 24, scalar) readonly buffer Emitters
{
    GpuEmitter emitters[];
};

/// Where each screen tile's sprites begin, with a sentinel so the last tile needs no special case.
///
/// `Rtx::SpriteTiles` says why the layer is binned per tile and the emitters are not.
layout(set = 0, binding = 30, scalar) readonly buffer SpriteTileOffsets
{
    uint spriteTileOffsets[];
};

/// Every tile's sprites, run together in tile order and ascending inside each run — which is the
/// order the march used to walk them in, and so the order they still composite in.
layout(set = 0, binding = 31, scalar) readonly buffer SpriteTileIndices
{
    uint spriteTileIndices[];
};

/// Every texture the scene loaded, indexed by the slot a material, a layer or an emitter names.
///
/// **A slot is qualified where it indexes and never where it is passed.** Neighbouring lanes hit
/// different materials over most of a frame, so the descriptor read has to be a waterfall — and what
/// tells the driver to emit one is a `NonUniform` decoration on the access chain itself.
/// `nonuniformEXT` applied to a function *argument* decorates the argument and stops there: the
/// chain built inside the callee comes out bare, and the driver may then read one lane's descriptor
/// for the whole wave. That is a wrong texture on some lanes of some waves, which looks like nothing
/// at all until it does. Measured before this rule, 28 of the 44 chains into this array were
/// undecorated and every one of them was on the surface path; `spirv-val` passes either way and the
/// validation layers say nothing.
layout(set = 1, binding = 0) uniform sampler2D textures[];

// **A buffer and not a push constant.** The frame's description passed 256 bytes, which is every
// byte `maxPushConstantsSize` promises on this hardware; `VisibilityPass` writes it into a buffer of
// its own instead. The name and the fields are the ones the push block had, so nothing that reads
// `camera` knows the difference.
//
// **Uniform and not storage**, which is worth 0.14 ms of the trace at Balmora: every pixel reads
// half of these fields several times over, and a uniform block is promoted to a constant bank the
// way the push constants it replaces were, where a storage buffer is a memory read like any other.
layout(set = 0, binding = 25, scalar) uniform Frame
{
    VisibilityConstants frame;
};

#endif
