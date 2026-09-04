// `#pragma once` everywhere else in this tree, and an include guard here for the reason
// `components/rtx/shaders/portable.h` gives.
#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_BINDINGS_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_BINDINGS_GLSL

// Everything the trace is handed: six bindings, and every table the scene owns reached through an
// address the frame block carries.
//
// **The frame's own block carries what is not a table, and where every table is.** The camera, the
// sky, the sea's moments and where the lamps were binned ride there because a record that is one
// row belongs in the block rather than in a descriptor of its own. The tables ride there as
// `GpuTables`, one address apiece, because seventeen storage-buffer bindings pushed twice a frame
// were seventeen places a layout and a write could disagree; `scene.h` says the rest.
//
// **A table is read through a function and never through its reference.** `instanceAt`, `lightAt`
// and their siblings construct the reference from the block and index it, so the alignment each
// reference claims is stated once, beside the table it is for. None of them takes `nonuniformEXT`:
// that decoration is for an index into a descriptor array, and a reference is not one, however the
// index into the table was reached.
//
// **The declarations, and `bindings.h` next door holds the numbers** — set zero's are a fact shared
// with `VisibilityPass`, which writes the same slots. What each channel is *for* is written here,
// beside the thing itself.
//
// **Four sets, by who made what they name.** Set zero is the frame and what it points at, pushed;
// set one is the bindless textures a scene owns; set two is the channels a `GBuffer` owns, numbered
// by `gbuffer.h`, which `GBuffer` reads too; set three is the air a `FogVolume` holds. The split is
// not tidiness — the device allows 32 push descriptors and this had reached exactly 32, so every
// list that keeps growing moved to the owner that already holds it.

#include "bindings.h"
#include "gbuffer.h"
#include "scene.h"
#include "visibility.h"
#include "wave.h"

#include "texturearray.glsl"

layout(set = 0, binding = BIND_SCENE) uniform accelerationStructureEXT sceneTop;

// Set two, in the order it is bound.

/// Everything already resolved: direct light, emission, the sky, water, and the fog over all of it.
layout(set = 2, binding = CHANNEL_DIRECT, GBUFFER_RADIANCE) uniform writeonly image2D direct;

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
layout(set = 2, binding = CHANNEL_INDIRECT, GBUFFER_RADIANCE) uniform writeonly image2D indirect;

/// The surface's own diffuse albedo, and nothing else.
///
/// What the composite multiplies the bounce back in by, and what Ray Reconstruction demodulates the
/// diffuse half of a pixel by. Zero where there is no diffuse response at all — the sky, and the
/// water, which answers a ray with a reflection and a refraction and no Lambert term.
layout(set = 2, binding = CHANNEL_ALBEDO, GBUFFER_ALBEDO) uniform writeonly image2D albedo;

/// The specular albedo, which is what an upscaler demodulates the mirrored half of a pixel by.
///
/// **Zero wherever the shading was Lambert, which is every solid surface this renderer has.** That
/// is a statement about the shading model and not a placeholder: nothing here answers a ray with a
/// specular lobe except the water, so nothing else has a specular albedo to report. Half floats,
/// because an albedo is a fraction that is never accumulated — the argument for full floats on the
/// radiance channels does not reach here.
layout(set = 2, binding = CHANNEL_SPECULAR, GBUFFER_ALBEDO) uniform writeonly image2D specular;

/// The shading normal in `xyz` and the surface's roughness in `w`.
///
/// **The normal the shading actually used**, which for water is the wave's rather than the plane's
/// — a rippled surface described as a flat one is reconstructed as a flat one. A ray that hit
/// nothing writes a zero normal, which no surface can be mistaken for.
layout(set = 2, binding = CHANNEL_GUIDE, GBUFFER_GUIDE) uniform writeonly image2D guide;

/// Where each surface stood on the previous frame's screen, less where it stands on this one.
layout(set = 2, binding = CHANNEL_MOTION, GBUFFER_MOTION) uniform writeonly image2D motion;

/// Clip depth in `r`, for whatever upscales the frame, and the distance from the eye in `g`, for
/// whatever filters it. Two questions, and one number cannot answer both.
layout(set = 2, binding = CHANNEL_DEPTH, GBUFFER_DEPTH) uniform writeonly image2D depth;

/// Where what the water reflects stood on the previous frame's screen, in pixels. Nought everywhere
/// that is not water reflecting a surface.
layout(set = 2, binding = CHANNEL_REFLECTION_MOTION, GBUFFER_MOTION) uniform writeonly image2D reflectionMotion;

/// Where a sprite reached, as one or nought.
///
/// **The one thing in the frame that carries no motion of its own.** One motion vector is written
/// per pixel, from the surface a primary ray hit, so every emitter — rain, snow, ash, smoke — is
/// reprojected with whatever geometry stands behind it. This is what tells the upscaler which pixels
/// those are, and it is free: the composite below already knows what the sprites left.
layout(set = 2, binding = CHANNEL_PARTICLE_MASK, GBUFFER_MASK) uniform writeonly image2D particleMask;

/// Where the past is not worth carrying forward, from nought to one.
///
/// The sprites above, and the water with them, for the reason `GBuffer::getBiasMask` gives.
layout(set = 2, binding = CHANNEL_BIAS_MASK, GBUFFER_MASK) uniform writeonly image2D biasMask;

/// How much of the star field this pixel still shows, per channel — everything the trace put between
/// the field and the eye, multiplied together.
///
/// **The field is drawn by a pass that can see none of this.** `ToneConstants::mStars` says why it
/// is drawn there and not here; what it costs is that the pass has no moons, no cloud deck, no
/// window pane, no water and no fog in front of the sky it is adding stars to. So the trace hands it
/// the one number that carries all of them: `skyRadiance`'s `shown` times the path's own
/// transmittance. Nought on every pixel that hit something, which is also how that pass knows.
layout(set = 2, binding = CHANNEL_STARS_SHOWN, GBUFFER_STARS) uniform writeonly image2D starsShown;

/// What the eye sees the frame through: the sprites, and the haze they stand in front of.
///
/// **Premultiplied, so the composite is `layer + (1 - opacity) * behind`.** `visibility.rgen` says
/// how that was measured, and what a straight colour drew instead.
layout(set = 2, binding = CHANNEL_TRANSPARENCY, GBUFFER_LAYER) uniform writeonly image2D transparency;

/// How much of the pixel that layer covers. A flame covers nothing and still writes a radiance.
///
/// **Three channels for one number, and it is not waste.** Written to a one-channel image the
/// upscaler read it as a colour — coverage in red and nought in green and blue — so it dimmed the
/// frame's red where a puff stood and let its green and blue through whole. A chimney's grey smoke
/// came out cyan, and so did a splash and a plume. The two masks beside this one are single-channel
/// and are read as scalars; this one is not, and the only way to find that out was to look.
layout(set = 2, binding = CHANNEL_TRANSPARENCY_OPACITY, GBUFFER_LAYER_OPACITY) uniform writeonly image2D transparencyOpacity;

/// Where the layer stood on the last frame's screen, which is not where the surface behind it stood.
layout(set = 2, binding = CHANNEL_TRANSPARENCY_MOTION, GBUFFER_MOTION) uniform writeonly image2D transparencyMotion;

// One atomic per hit on a single address, which looks like contention and measures as nothing: at
// 3840x2160 over Seyda Neen the trace runs 0.57-0.79 ms, and a subgroup reduction in place of this
// ran 0.65-0.78 for an identical count. Only 5.5% of rays hit, and the reduction would have cost the
// device a subgroup-arithmetic requirement it does not otherwise need. Measure again if a pass ever
// hits most of its pixels.
layout(set = 0, binding = BIND_HITS) buffer HitCount
{
    uint hits;

    /// The see-through surfaces those rays crossed, summed over the frame, and the most any one ray
    /// crossed. `COUNT_CROSSINGS` says what they are for and why they are not counted with the hits.
    ///
    /// **The worst ray beside the mean**, because a mean over a frame answers a question nobody
    /// asked: a cloud that fills a tenth of the picture divides its own depth by ten, and what a
    /// march costs is what the deepest ray in it does.
    uint crossings;
    uint crossingsMost;
};

// **A buffer and not a push constant.** The frame's description passed 256 bytes, which is every
// byte `maxPushConstantsSize` promises on this hardware; `VisibilityPass` writes it into a buffer of
// its own instead. The name and the fields are the ones the push block had, so nothing that reads
// `camera` knows the difference.
//
// **Uniform and not storage**, which is worth 0.14 ms of the trace at Balmora: every pixel reads
// half of these fields several times over, and a uniform block is promoted to a constant bank the
// way the push constants it replaces were, where a storage buffer is a memory read like any other.
//
// **Declared before the tables, because the tables are reached through it.**
layout(set = 0, binding = BIND_FRAME, scalar) uniform Frame
{
    VisibilityConstants frame;
};

// The scene's tables, each a reference constructed from the address `frame.mTables` carries.
//
// **The alignment each reference claims is `scene.h`'s to state**, beside the rows it is about:
// `TABLE_ALIGN_LAYERS` where a 48-byte row puts two `vec4` on sixteen, `TABLE_ALIGN_BLOCKS` for a
// table of eight-byte addresses, and `TABLE_ALIGN_ROWS` everywhere a row or an element is only
// four-aligned. The host asserts every address against the same three numbers before it writes the
// block, so a claim here is a claim something checks.
//
// **Not `restrict`, though it would be true.** Tried on every reference: it gave back a hundredth
// or two of the four the address path costs the guild's trace, inside the noise, and it moved two
// interiors — one by nineteen levels on seventy-six pixels — where the compiled shape shifted a lamp
// pick. `GpuTables` carries the cost it did not recover.

// The vertex attributes and the indices, as lists of blocks.
//
// **A block is allocated once at its full size and never moved**, so a scene that grows keeps every
// address already handed out and every acceleration structure built from one stays valid — which is
// what lets a cell arriving append rather than rebuild the world. What the frame carries is *where*
// the blocks are; a global id resolves to one of them and an offset inside it. `Rtx::SceneDesc` never
// lets a mesh's run straddle a block, and both sizes are powers of two, so that is a shift and a
// mask.
//
// The alignment is four: a twelve-byte element at an arbitrary index is only ever float-aligned.
layout(buffer_reference, scalar, buffer_reference_align = TABLE_ALIGN_ROWS) readonly buffer NormalBlock
{
    vec3 at[];
};

layout(buffer_reference, scalar, buffer_reference_align = TABLE_ALIGN_ROWS) readonly buffer TexCoordBlock
{
    vec2 at[];
};

layout(buffer_reference, scalar, buffer_reference_align = TABLE_ALIGN_ROWS) readonly buffer IndexBlock
{
    uint at[];
};

/// Where a blocked table's blocks start: a table of addresses, one per block.
layout(buffer_reference, scalar, buffer_reference_align = TABLE_ALIGN_BLOCKS) readonly buffer BlockTable
{
    uint64_t at[];
};

// The block a global id lives in.
//
// **Resolved once for a triangle and not once for a corner.** A mesh's run never straddles a block,
// so its three corners and its three indices share one — and the table read that finds it is the
// half of every attribute fetch that is the same for all three. `geometry.glsl` is what takes the
// block and reads the corners out of it.
NormalBlock normalBlockOf(uint vertex)
{
    return NormalBlock(BlockTable(frame.mTables.mNormalBlocks).at[vertex / VERTEX_BLOCK]);
}

TexCoordBlock texCoordBlockOf(uint vertex)
{
    return TexCoordBlock(BlockTable(frame.mTables.mTexCoordBlocks).at[vertex / VERTEX_BLOCK]);
}

IndexBlock indexBlockOf(uint element)
{
    return IndexBlock(BlockTable(frame.mTables.mIndexBlocks).at[element / INDEX_BLOCK]);
}

layout(buffer_reference, scalar, buffer_reference_align = TABLE_ALIGN_ROWS) readonly buffer MeshTable
{
    GpuMesh at[];
};

layout(buffer_reference, scalar, buffer_reference_align = TABLE_ALIGN_ROWS) readonly buffer InstanceTable
{
    GpuInstance at[];
};

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

layout(buffer_reference, scalar, buffer_reference_align = TABLE_ALIGN_ROWS) readonly buffer LightTable
{
    GpuLight at[];
};

/// A list of `uint`: the light grid's, and the sprite tiles'.
layout(buffer_reference, scalar, buffer_reference_align = TABLE_ALIGN_ROWS) readonly buffer IndexList
{
    uint at[];
};

layout(buffer_reference, scalar, buffer_reference_align = TABLE_ALIGN_ROWS) readonly buffer BlueNoiseTable
{
    float at[];
};

layout(buffer_reference, scalar, buffer_reference_align = TABLE_ALIGN_ROWS) readonly buffer SpriteTable
{
    GpuSprite at[];
};

layout(buffer_reference, scalar, buffer_reference_align = TABLE_ALIGN_ROWS) readonly buffer EmitterTable
{
    GpuEmitter at[];
};

GpuMesh meshAt(uint index)
{
    return MeshTable(frame.mTables.mMeshes).at[index];
}

GpuInstance instanceAt(uint index)
{
    return InstanceTable(frame.mTables.mInstances).at[index];
}

GpuMaterial materialAt(uint index)
{
    return MaterialTable(frame.mTables.mMaterials).at[index];
}

GpuLayer layerAt(uint index)
{
    return LayerTable(frame.mTables.mLayers).at[index];
}

float maskAt(uint index)
{
    return MaskTable(frame.mTables.mMasks).at[index];
}

GpuLight lightAt(uint index)
{
    return LightTable(frame.mTables.mLights).at[index];
}

/// The light grid's list: where each cell's run starts, counted from the front of the list, with a
/// sentinel so the last cell's end needs no special case — and after those starts, every cell's
/// lamps run together in cell order. Cell `c`'s lamps are `at[at[c]] .. at[at[c + 1]]`.
/// `Rtx::LightGrid` says why one list and not two.
uint lightListAt(uint slot)
{
    return IndexList(frame.mTables.mLightList).at[slot];
}

/// The blue-noise tile, `RANDOM_STREAMS` channels interleaved per pixel. See `Rtx::BlueNoise`.
float blueNoiseAt(uint index)
{
    return BlueNoiseTable(frame.mTables.mBlueNoise).at[index];
}

/// Every live particle in the scene, one emitter's run after another's.
GpuSprite spriteAt(uint index)
{
    return SpriteTable(frame.mTables.mSprites).at[index];
}

/// One sphere and one run of sprites per particle system, indexed by `GpuSprite::mEmitter`.
///
/// **No count beside it, because nothing walks these.** The buffer never shrinks, so its length
/// outlives the cell that filled it — and since the sprite tiles replaced the loop over emitters,
/// the only way in is from a sprite the tile named, which can only name one that is real.
GpuEmitter emitterAt(uint index)
{
    return EmitterTable(frame.mTables.mEmitters).at[index];
}

/// The sprite tiles' list, in the light grid's shape over the screen's tiles: where each tile's run
/// starts, then every tile's sprites run together in tile order and ascending inside each run —
/// which is the order the march used to walk them in, and so the order they still composite in.
///
/// **Made on the device, by `SpriteBinPass`, ahead of the trace.** `spriterects.comp` says why the
/// layer is binned per tile and the emitters are not, and `SPRITE_LIST_UNBINNED` what entry nought
/// holds on the frame whose runs did not fit.
uint spriteTileListAt(uint slot)
{
    return IndexList(frame.mTables.mSpriteTileList).at[slot];
}

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
/// previous frame left it — and beside it the three answers a ray each gave there: the sun's
/// transport in `r`, the lamp's seeing in `g` and the ambient's in `b`. These are the quantities
/// that reproject, so these are the ones a frame averages against.
layout(set = 3, binding = 0) uniform sampler3D fogWasScatter;
layout(set = 3, binding = 1) uniform sampler3D fogWasSunward;

/// The same two as this frame's scatter pass wrote them, which is what its integrate pass reads —
/// and what a puff of smoke reads at a point, `puffLight` being the one thing in the trace that
/// wants a froxel's own answer rather than a column's integral of it.
layout(set = 3, binding = 2) uniform sampler3D fogScatter;
layout(set = 3, binding = 3) uniform sampler3D fogSunward;

/// What every lamp puts into a froxel, per steradian and with nothing standing in the way — read by
/// the integrate pass beside the seeing above it, and by a puff for the same product.
layout(set = 3, binding = 4) uniform sampler3D fogLamps;

/// Both accumulated front to back, which is what a pixel reads. `a` of the first is what is left of
/// a ray at that depth; the second is the sun's transport alone, one channel.
layout(set = 3, binding = 5) uniform sampler3D fogVolumeAir;
layout(set = 3, binding = 6) uniform sampler3D fogVolumeSunward;

/// What each slice holds once everything that lights it is applied — `FogSlice`, as the two images
/// it packs into — which is what a pixel steps through from the last edge it passed to where its
/// surface stands.
layout(set = 3, binding = 7) uniform sampler3D fogSlice;
layout(set = 3, binding = 8) uniform sampler3D fogSliceSunward;

/// The same seven, as the pass that fills each one writes it.
layout(set = 3, binding = 9, FOG_VOLUME_FORMAT) uniform writeonly image3D fogScatterTarget;
layout(set = 3, binding = 10, FOG_VOLUME_FORMAT) uniform writeonly image3D fogSunwardTarget;
layout(set = 3, binding = 11, FOG_VOLUME_FORMAT) uniform writeonly image3D fogLampsTarget;
layout(set = 3, binding = 12, FOG_VOLUME_FORMAT) uniform writeonly image3D fogVolumeAirTarget;
layout(set = 3, binding = 13, r16f) uniform writeonly image3D fogVolumeSunwardTarget;
layout(set = 3, binding = 14, FOG_VOLUME_FORMAT) uniform writeonly image3D fogSliceTarget;
layout(set = 3, binding = 15, r16f) uniform writeonly image3D fogSliceSunwardTarget;

/// How far each column's ray runs before it meets a surface, which `fogdepth.comp` writes and the
/// scatter pass reads. **One storage binding for both**, because neither samples it: a column reads
/// its own texel and nothing between texels.
layout(set = 3, binding = 16, r32f) uniform image2D fogColumnDepth;

#endif
