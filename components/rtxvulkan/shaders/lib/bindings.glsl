// `#pragma once` everywhere else in this tree, and an include guard here for the reason
// `components/rtx/shaders/portable.h` gives.
#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_BINDINGS_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_BINDINGS_GLSL

// Everything the trace is handed, and the three accessors that resolve a global vertex
// or index id to the block it lives in.
//
// **The declarations, and `bindings.h` next door holds the numbers** — set zero's are a fact shared
// with `VisibilityPass`, which writes the same slots. What each channel is *for* is written here,
// beside the thing itself.
//
// **Four sets, by who made what they name.** Set zero is the frame and the scene's tables, pushed;
// set one is the bindless textures a scene owns; set two is the channels a `GBuffer` owns, and
// channel `i` of `GBuffer::everyChannel` is binding `i` there; set three is the air a `FogVolume`
// holds. The split is not tidiness — the device allows 32 push descriptors and this had reached
// exactly 32, so every list that keeps growing moved to the owner that already holds it.

#include "bindings.h"
#include "gbuffer.h"
#include "scene.h"
#include "visibility.h"
#include "wave.h"

#include "texturearray.glsl"

layout(set = 0, binding = BIND_SCENE) uniform accelerationStructureEXT sceneTop;

/// Everything already resolved: direct light, emission, the sky, water, and the fog over all of it.
layout(set = 2, binding = 0, GBUFFER_RADIANCE) uniform writeonly image2D direct;

// One atomic per hit on a single address, which looks like contention and measures as nothing: at
// 3840x2160 over Seyda Neen the trace runs 0.57-0.79 ms, and a subgroup reduction in place of this
// ran 0.65-0.78 for an identical count. Only 5.5% of rays hit, and the reduction would have cost the
// device a subgroup-arithmetic requirement it does not otherwise need. Measure again if a pass ever
// hits most of its pixels.
layout(set = 0, binding = BIND_HITS) buffer HitCount
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

layout(set = 0, binding = BIND_NORMALS, scalar) readonly buffer NormalBlocks
{
    uint64_t normalBlocks[];
};

layout(set = 0, binding = BIND_TEXCOORDS, scalar) readonly buffer TexCoordBlocks
{
    uint64_t texCoordBlocks[];
};

layout(set = 0, binding = BIND_INDICES, scalar) readonly buffer IndexBlocks
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

layout(set = 0, binding = BIND_MESHES, scalar) readonly buffer Meshes
{
    GpuMesh meshes[];
};

layout(set = 0, binding = BIND_INSTANCES, scalar) readonly buffer Instances
{
    GpuInstance instances[];
};

layout(set = 0, binding = BIND_MATERIALS, scalar) readonly buffer Materials
{
    GpuMaterial materials[];
};

layout(set = 0, binding = BIND_LAYERS, scalar) readonly buffer Layers
{
    GpuLayer layers[];
};

layout(set = 0, binding = BIND_MASKS, scalar) readonly buffer Masks
{
    float masks[];
};

layout(set = 0, binding = BIND_LIGHTS, scalar) readonly buffer Lights
{
    GpuLight lights[];
};

/// Where each cell's lamps start, with a sentinel so the last cell's end needs no special case.
layout(set = 0, binding = BIND_LIGHT_OFFSETS, scalar) readonly buffer LightOffsets
{
    uint lightOffsets[];
};

/// Every cell's lamps, run together in cell order.
layout(set = 0, binding = BIND_LIGHT_INDICES, scalar) readonly buffer LightIndices
{
    uint lightIndices[];
};

/// The blue-noise tile, `RANDOM_STREAMS` channels interleaved per pixel. See `Rtx::BlueNoise`.
layout(set = 0, binding = BIND_BLUE_NOISE, scalar) readonly buffer BlueNoiseTile
{
    float blueNoise[];
};

/// Clip depth in `r`, for whatever upscales the frame, and the distance from the eye in `g`, for
/// whatever filters it. Two questions, and one number cannot answer both.
layout(set = 2, binding = 6, GBUFFER_DEPTH) uniform writeonly image2D depth;

/// Where the lamps were binned, which is scene geometry rather than camera geometry.
layout(set = 0, binding = BIND_LIGHT_GRID, scalar) readonly buffer LightGridBlock
{
    GpuLightGrid grid;
};

/// Where each surface stood on the previous frame's screen, less where it stands on this one.
layout(set = 2, binding = 5, GBUFFER_MOTION) uniform writeonly image2D motion;

/// What each texture already has painted into it, `SHADING_EXTENT` squared factors apiece and one
/// texture after another. A texture with no estimate holds ones.
layout(set = 0, binding = BIND_SHADING, scalar) readonly buffer ShadingMaps
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
layout(set = 2, binding = 1, GBUFFER_RADIANCE) uniform writeonly image2D indirect;

/// The surface's own diffuse albedo, and nothing else.
///
/// What the composite multiplies the bounce back in by, and what Ray Reconstruction demodulates the
/// diffuse half of a pixel by. Zero where there is no diffuse response at all — the sky, and the
/// water, which answers a ray with a reflection and a refraction and no Lambert term.
layout(set = 2, binding = 2, GBUFFER_ALBEDO) uniform writeonly image2D albedo;

/// The shading normal in `xyz` and the surface's roughness in `w`.
///
/// **The normal the shading actually used**, which for water is the wave's rather than the plane's
/// — a rippled surface described as a flat one is reconstructed as a flat one. A ray that hit
/// nothing writes a zero normal, which no surface can be mistaken for.
layout(set = 2, binding = 4, GBUFFER_GUIDE) uniform writeonly image2D guide;

/// The specular albedo, which is what an upscaler demodulates the mirrored half of a pixel by.
///
/// **Zero wherever the shading was Lambert, which is every solid surface this renderer has.** That
/// is a statement about the shading model and not a placeholder: nothing here answers a ray with a
/// specular lobe except the water, so nothing else has a specular albedo to report. Half floats,
/// because an albedo is a fraction that is never accumulated — the argument for full floats on the
/// radiance channels does not reach here.
layout(set = 2, binding = 3, GBUFFER_ALBEDO) uniform writeonly image2D specular;

/// Where a sprite reached, as one or nought.
///
/// **The one thing in the frame that carries no motion of its own.** One motion vector is written
/// per pixel, from the surface a primary ray hit, so every emitter — rain, snow, ash, smoke — is
/// reprojected with whatever geometry stands behind it. This is what tells the upscaler which pixels
/// those are, and it is free: the composite below already knows what the sprites left.
layout(set = 2, binding = 8, GBUFFER_MASK) uniform writeonly image2D particleMask;

/// Where the past is not worth carrying forward, from nought to one.
///
/// The sprites above, and the water with them, for the reason `GBuffer::getBiasMask` gives.
layout(set = 2, binding = 9, GBUFFER_MASK) uniform writeonly image2D biasMask;

/// Where what the water reflects stood on the previous frame's screen, in pixels. Nought everywhere
/// that is not water reflecting a surface.
layout(set = 2, binding = 7, GBUFFER_MOTION) uniform writeonly image2D reflectionMotion;

/// How much of the star field this pixel still shows, per channel — everything the trace put between
/// the field and the eye, multiplied together.
///
/// **The field is drawn by a pass that can see none of this.** `ToneConstants::mStars` says why it
/// is drawn there and not here; what it costs is that the pass has no moons, no cloud deck, no
/// window pane, no water and no fog in front of the sky it is adding stars to. So the trace hands it
/// the one number that carries all of them: `skyRadiance`'s `shown` times the path's own
/// transmittance. Nought on every pixel that hit something, which is also how that pass knows.
layout(set = 2, binding = 10, GBUFFER_STARS) uniform writeonly image2D starsShown;

/// Every live particle in the scene, one emitter's run after another's.
layout(set = 0, binding = BIND_SPRITES, scalar) readonly buffer Sprites
{
    GpuSprite sprites[];
};

/// One sphere and one run of sprites per particle system, indexed by `GpuSprite::mEmitter`.
///
/// **No count beside it, because nothing walks these.** The buffer never shrinks, so its length
/// outlives the cell that filled it — and since the sprite tiles replaced the loop over emitters,
/// the only way in is from a sprite the tile named, which can only name one that is real.
layout(set = 0, binding = BIND_EMITTERS, scalar) readonly buffer Emitters
{
    GpuEmitter emitters[];
};

/// Where each screen tile's sprites begin, with a sentinel so the last tile needs no special case.
///
/// `Rtx::SpriteTiles` says why the layer is binned per tile and the emitters are not.
layout(set = 0, binding = BIND_SPRITE_TILE_OFFSETS, scalar) readonly buffer SpriteTileOffsets
{
    uint spriteTileOffsets[];
};

/// Every tile's sprites, run together in tile order and ascending inside each run — which is the
/// order the march used to walk them in, and so the order they still composite in.
layout(set = 0, binding = BIND_SPRITE_TILE_INDICES, scalar) readonly buffer SpriteTileIndices
{
    uint spriteTileIndices[];
};


// **A buffer and not a push constant.** The frame's description passed 256 bytes, which is every
// byte `maxPushConstantsSize` promises on this hardware; `VisibilityPass` writes it into a buffer of
// its own instead. The name and the fields are the ones the push block had, so nothing that reads
// `camera` knows the difference.
//
// **Uniform and not storage**, which is worth 0.14 ms of the trace at Balmora: every pixel reads
// half of these fields several times over, and a uniform block is promoted to a constant bank the
// way the push constants it replaces were, where a storage buffer is a memory read like any other.
layout(set = 0, binding = BIND_FRAME, scalar) uniform Frame
{
    VisibilityConstants frame;
};

// The sea, as the tiles `WavePass` synthesised it into. One texture apiece per cascade, sampled
// rather than loaded, because the level a ray cone reaches is what a water pixel asks for.
//
// **Three textures and not one, because the third is a square that has to be averaged apart from
// what it is the square of.** The mean of `tr(H)^2` over a footprint and the square of the mean of
// `tr(H)` are different numbers, and their difference is the curvature the cone threw away.

/// The two slopes, their own second moment, and the elevation squared.
layout(set = 0, binding = BIND_WAVE_SURFACE) uniform sampler2D waveSurface[WAVE_CASCADES];

/// The three curvatures.
layout(set = 0, binding = BIND_WAVE_CURVATURE) uniform sampler2D waveCurvature[WAVE_CASCADES];

/// The fog's fractal field, drawn once for the life of the device and read at three world scales.
///
/// **Wrapping, mipped, and two channels.** `.x` is the shape a coverage band is cut out of and `.y`
/// is a second field decorrelated from it — read together they are the displacement the finer scales
/// are sampled at, which is a vector out of one fetch rather than two fetches at two places.
///
/// **A volume and not a ground plan**, for the reason `FOG_FIELD_SIZE` gives: a field with no third
/// axis holds one value all the way up, so every bank in it is a column.
///
/// `Rtx::bakeFogNoise` says what is in it, and why every level of the chain carries one spread.
layout(set = 0, binding = BIND_FOG_FIELD) uniform sampler3D fogField;

// The air in front of the eye, integrated once for a block of pixels rather than once per pixel.
// `Rtx::FogVolume` says what each image holds and why there are three pairs of them.
//
// **A set of its own, which is the set `GBuffer` already argued for.** Set zero is pushed, the
// device allows 32 push descriptors and this renderer had reached exactly that once — so images
// belonging to a camera's size go with the owner that holds them.
//
// **Each image is named twice wherever a pass both reads and writes it**, because Vulkan has no
// descriptor that is both. Which physical image the first two pairs name swaps every frame:
// `FogVolume::getSet` hands over the set whose history is what the last frame wrote.

/// What the air scatters at a point in `rgb` and its extinction per world unit in `a`, as the
/// previous frame left it — and the sun's transport there beside it. These are the quantities that
/// reproject, so these are the ones a frame averages against.
layout(set = 3, binding = 0) uniform sampler3D fogWasScatter;
layout(set = 3, binding = 1) uniform sampler3D fogWasSunward;

/// The same two, as this frame writes them.
layout(set = 3, binding = 2, FOG_VOLUME_FORMAT) uniform writeonly image3D fogScatterTarget;
layout(set = 3, binding = 3, FOG_VOLUME_FORMAT) uniform writeonly image3D fogSunwardTarget;

/// Both accumulated front to back, which is what a pixel reads. `a` of the first is what is left of
/// a ray at that depth.
layout(set = 3, binding = 4, FOG_VOLUME_FORMAT) uniform writeonly image3D fogVolumeAirTarget;
layout(set = 3, binding = 5, FOG_VOLUME_FORMAT) uniform writeonly image3D fogVolumeSunwardTarget;

layout(set = 3, binding = 6) uniform sampler3D fogVolumeAir;
layout(set = 3, binding = 7) uniform sampler3D fogVolumeSunward;

#endif
