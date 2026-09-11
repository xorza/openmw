# RTX shader and binding review

## Scope

This review covers every file under `components/rtx/shaders/` and
`components/rtxvulkan/shaders/`. It also covers the host code that declares set layouts,
writes descriptors and records dispatches: `dispatch.hpp`, `setlayout.*`,
`pipelinelayout.*`, `computepipeline.*`, `tracepipeline.*`, `gbuffer.*`, `fogvolume.*`
and every `*pass.cpp`.

The tree holds 14239 lines of shader text. 5224 of those lines are code. The rest is
comment. The comment density is high and the rationale is good. Almost every number has a
measurement behind it. The findings below are therefore about structure and about traffic,
not about magic numbers.

Nothing in this file was changed. This is a report.

**Read `.notes/rtx-shader-fixes.md` beside it.** That file holds the proposal, the plan and
what happened when the plan was run — including the two findings below that the
measurements refuted. F1 is one of them. Its ranking here is wrong and section 7 of that
file says why.

## Ranked summary

| ID | Finding | Kind | Expected gain |
|----|---------|------|---------------|
| F1 | Shadow rays sample every cutout texture at mip 0 | Traffic | High |
| F2 | `GpuInstance` is 60 bytes wide for 12 hot ones | Traffic | Measure |
| F3 | The à-trous pass reads 375 texels per pixel with no tile in shared memory | Traffic | High |
| F4 | The fog integrator refetches each 3x3 tent tap nine times | Traffic | High |
| F5 | `GBUFFER_RADIANCE` and the composite target are FP32 | Traffic | High |
| F6 | `committedHit` and `resolveFor` resolve the same triangle twice | Compute | Medium |
| F7 | `gather` builds the sky source three times for one ray | Compute | Medium |
| F8 | `textureSize` is called inside per-sprite and per-cascade loops | Compute | Medium |
| F9 | `fogColumn` recomputes a ray-constant exponential for every sprite | Compute | Medium |
| F10 | The peel loop integrates the whole fog column once per layer | Compute | Medium |
| F11 | Fifteen hit groups exist where three and a payload word would do | Simplify | Medium |
| F12 | `tangentTo` and `squareTo` branch where a branch-free basis exists | Branch | Medium |
| F13 | Small arrays with a computed index can spill to local memory | Branch | Medium |
| F14 | Four passes emit one barrier per image where `Barriers` batches | Traffic | Medium |
| F15 | The DLSS layer channels are allocated when DLSS is off | Traffic | Low |
| F16 | `paintedOver` calls `pow` to compute an identity | Compute | Low |
| F17 | `pow` pairs share a logarithm that is taken twice | Compute | Low |
| F18 | Read-only inputs use `imageLoad` and not `texelFetch` | Traffic | Low |
| F19 | `wavecompose` declares a fourth binding that nothing uses | Defect | — |
| F20 | Four library files are absent from `RTX_SHADER_LIB` | Defect | — |
| F21 | Sixteen headers repeat the same host alias block | Unify | — |
| F22 | Five headers define and undefine a `uint64` macro | Unify | — |
| F23 | Each compute shader spells the bounds test differently | Symmetry | — |
| F24 | `tone.h` pulls the whole scene description for one struct | Unify | — |
| F25 | The candidate walk exists in two forms | Unify | — |
| F26 | The bin passes use shared words where a subgroup ballot fits | Compute | Measure |
| F27 | `spriteshade` sorts in device memory and reads with one lane | Compute | Measure |
| F28 | `resolveFor` fetches the diffuse texel twice for a pane | Compute | Measure |
| F29 | Wave pass and exposure pass declare bindings by hand | Symmetry | — |
| F30 | Two masks occupy two single-channel images | Traffic | Low |

---

## A. Memory traffic

### F1. Shadow rays sample every cutout texture at mip 0

**Where.** `components/rtxvulkan/shaders/lib/traversal.glsl:365`, inside `lightThrough`.

**What.** `lightThrough` passes `0.0` as the cone width to `RTX_RESOLVE`. That value
reaches `coneLod` through `candidateStops`. `coneLod` returns `0.0` for a width of zero.
Every alpha test on a shadow ray therefore reads the finest mip.

**Why it costs.** A leaf card 200 metres away covers a fraction of a texel. The trace
still reads a 1024x1024 texture at level 0. The read misses the texture cache almost
always. `camera.h` states this failure mode for the primary ray and solves it there. The
shadow ray kept the old behaviour.

**Change.** Give `lightThrough` a footprint parameter. Every caller has one. `gather`
holds `footprint` and already hands it to `lightThroughWater`. `ambientReaching` can take
the surface footprint. `lampVisible` can take it from the reservoir.

**Risk.** The picture changes where a coarse mip softens a cutout edge in shadow. That is
the correct answer, not a regression. Run `repeatable.sh --pairs=10` after the change.

### F2. `GpuInstance` is 60 bytes wide for 12 hot ones

**Where.** `components/rtx/shaders/scene.h:424`, `GpuInstance`. Read at
`traversal.glsl:130` in `candidateStops`, at `traversal.glsl:284` in `committedHit`, and
at `traversal.glsl:579` in `resolveFor`.

**What.** `GpuInstance` is 60 bytes. `vec4 mMotion[3]` is 48 of those. The three hot
readers want `mMesh`, `mMaterial` and `mOpacity`, which are the first 12 bytes. Only
`movedBy` in `reproject.glsl:25` wants the motion, and it runs once per pixel in the ray
generation shader alone.

**What the compiled module says.** The optimizer already narrows the load. In
`build-debug/resources/rtx/shaders/visibility.rahit.spv` the access chain for the row is
followed by two four-byte member loads at offsets 0 and 4, and by no load of the motion.
The first claim to make here is therefore *not* that 60 bytes are read.

**What remains.** Two things. The row stride is 60 bytes, so one 32-byte sector holds 12
useful bytes and 20 bytes of motion. The table is five times larger than the hot part, so
it holds five times as much of the cache. Neither is a certain win on a card with this
much L2, which is why the entry is marked "Measure".

**Change.** Move `mMotion` to a table of its own. Add `mMotions` to `GpuTables` and a
`motionAt(index)` accessor beside `instanceAt`. `GpuInstance` then becomes 12 bytes, and
two rows and a half fit in one sector.

**How to decide.** Take the reading at the Balmora mages' guild, where the candidate count
is highest, and at `seyda-neen-ship`. Interleave the legs. If the trace does not move,
keep the smaller row anyway only if it also simplifies the upload, and say so.

**Risk.** Low. One new table address, one new alignment claim, one more line in
`everyTableAddressed`.

### F3. The à-trous pass reads 375 texels per pixel with no tile in shared memory

**Where.** `components/rtxvulkan/shaders/atrous.comp:117` to `:146`.

**What.** Each of the 25 taps reads `guide`, `depth` and `source`. That is 75 loads per
pixel per level. `ATROUS_LEVELS` is 5, so one pixel costs 375 image loads. Each tap also
calls `positionAt`, which calls `rayAt`. That is 125 normalizations per pixel.

**Why it costs.** An 8x8 workgroup at `mStep` of one needs a 12x12 halo. That halo holds
144 texels. The group reads 64 x 75, which is 4800 loads for 432 distinct values. The
factor is eleven. At `mStep` of two the halo is 16x16 and the factor is still above six.

**Change.** Stage `guide` and `depth` into shared memory for the first three levels. Keep
the current path for the two coarsest levels, where the halo outgrows the tile. This is
the standard shape for an SVGF filter. The reference implementations from NVIDIA and from
the original SVGF paper both tile the guide.

**Risk.** Medium. The tile adds shared memory and two barriers. The result must stay
bit-exact, so compare the frame hash for the picture as well as for the scene.

### F4. The fog integrator refetches each 3x3 tent tap nine times

**Where.** `components/rtxvulkan/shaders/fogintegrate.comp:81` to `:112`, `sliceAt`.

**What.** One thread owns one column. It walks 64 slices. At each slice it reads a 3x3
tent from `fogScatter` and from `fogSunward`. That is 18 `texelFetch` calls per slice, or
about 1170 per column.

**Why it costs.** Neighbouring threads in the 8x8 workgroup read overlapping 3x3
neighbourhoods. A 10x10 tile per slice holds 100 texels. The group fetches 64 x 9, which
is 576 for those 100 values. The factor is near six on both images.

**Change.** Load a 10x10 tile of `fogScatter` and `fogSunward` per slice into shared
memory. The slice loop then reads the tile. A double buffer over two slices removes one of
the two barriers per slice.

**Risk.** Medium. Shared memory for 100 x `vec4` x 2 is 3.2 kB, which is comfortable.
The arithmetic is unchanged, so the frame hash must not move.

### F5. `GBUFFER_RADIANCE` and the composite target are FP32

**Where.** `components/rtx/shaders/gbuffer.h:49`, `components/rtxvulkan/tracechain.cpp:57`,
`components/rtxvulkan/vulkanrenderer.cpp:267`.

**What.** `Channel::Direct` and `Channel::Indirect` are `VK_FORMAT_R32G32B32A32_SFLOAT`.
The composite output and the upscaled output use the same format.

**Why it costs.** The two radiance channels write 32 bytes per pixel per frame. At
1920x1080 and 60 fps that is 4 GB/s of store traffic for those two alone. The composite
image at 3840x2160 is 133 MB, and the tone pass reads all of it. The accumulator already
keeps its history in `rgba16f` through `ACCUMULATE_COLOUR`, so the indirect channel is
already narrowed one pass later.

**Why FP16 is enough.** `MAX_SUN_RADIANCE` is 1000 and `EXPOSURE_BLACK` is 1e-4. FP16
reaches 65504 and holds 1e-4 with three decimal digits. The exposure is applied after the
composite, so no value in these images approaches the FP16 limit.

**Change.** Set `GBUFFER_RADIANCE` to `rgba16f`. Set the composite target and the upscaled
target to `VK_FORMAT_R16G16B16A16_SFLOAT`. Leave the offline `sum` image at FP32, because
it accumulates hundreds of frames and that is a different question.

**Risk.** Medium. The picture may move in the last bits. Take the reading with
`--exposure=1` and read `Rtx::Channel::Radiance` as floats, as the project file directs.

### F14. Four passes emit one barrier per image where `Barriers` batches

**Where.** `components/rtxvulkan/fogvolume.cpp` in `begin`, `depthTaken`, `scattered` and
`handOver`. `components/rtxvulkan/bloompass.cpp` in `record`.
`components/rtxvulkan/accumulatepass.cpp` in `record`.
`components/rtxvulkan/wavepass.cpp` in `record`.

**What.** Each of these loops over a set of images and calls `Image::transition` for each
one. Every call is one `vkCmdPipelineBarrier2`. `FogVolume::begin` alone emits eleven.
`GBuffer::begin` uses the `Barriers` class in `image.hpp:162` and emits one.

**Why it costs.** The class comment states the reason and cites NVIDIA's guidance. The
G-buffer follows it and these four do not. A frame emits about 30 extra barrier commands.
Each one is a potential sync point for the driver.

**Change.** Use `Barriers` in all four. The array holds 16, which covers every run here.

**Risk.** None. The batch emits the same dependencies in one command.

### F15. The DLSS layer channels are allocated when DLSS is off

**Where.** `components/rtxvulkan/gbuffer.cpp`, the `GBuffer` constructor.
`components/rtxvulkan/shaders/visibility.rgen:481`.

**What.** `Transparency`, `TransparencyOpacity` and `TransparencyMotion` are written only
when `frame.mLayerCompositedAfter` is not zero. That flag is `upscaling() ? 1 : 0`. The
three images are always created and always transitioned twice a frame.

**Why it costs.** 8 + 4 + 8 bytes per pixel, which is 41 MB at 1920x1080. Six of the 28
G-buffer barriers guard images that hold nothing.

**Change.** Give `GBuffer` a flag for the layer channels. Allocate a 1x1 stand-in where
the flag is false, the way `CompositePass::mNoSum` and `TonePass::mNoBloom` already do.
The set layout stays the same size, so the shader needs no change.

**Risk.** Low. The pattern exists in the tree twice.

### F18. Read-only inputs use `imageLoad` and not `texelFetch`

**Where.** `accumulate.comp`, `atrous.comp`, `composite.comp`, `tone.comp`,
`histogram.comp`.

**What.** These passes declare their inputs as `readonly image2D` and read them with
`imageLoad`. Every G-buffer channel already carries `VK_IMAGE_USAGE_SAMPLED_BIT`, and
`VK_IMAGE_LAYOUT_GENERAL` is legal for a sampled read.

**Why it costs.** On NVIDIA hardware an image load goes through the load-store path. A
texture fetch goes through the texture unit and its own cache. For a gather pattern like
the à-trous kernel the texture cache is the better fit. `fogintegrate.comp` already uses
`texelFetch`, so the tree holds both forms.

**Change.** Declare read-only inputs as `texture2D` with
`VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE`, and read them with `texelFetch`. Keep `imageLoad` only
where a pass reads and writes the same image, such as `bloomup.comp` and the composite
`sum`.

**Risk.** Low, but the gain is hardware-dependent. Measure the à-trous pass alone, where
the tap count is highest. Fold this into F3 and measure once.

### F30. Two masks occupy two single-channel images

**Where.** `gbuffer.h:111`, `CHANNEL_PARTICLE_MASK` and `CHANNEL_BIAS_MASK`.

**What.** Both are `r8`. `visibility.rgen` writes both every pixel. DLSS reads both.

**Why it costs.** Two images, two descriptors, two barriers and two `imageStore` calls
where one `rg8` image would serve. The saving is small. The comment on
`CHANNEL_TRANSPARENCY_OPACITY` explains why that one must stay three-channel, and the same
reasoning does not apply to these two because the upscaler reads them as scalars.

**Change.** Only take this if DLSS accepts two masks in one image. Check the NGX interface
first. If it does not, leave the two as they are and record why.

---

## B. Repeated computation

### F6. `committedHit` and `resolveFor` resolve the same triangle twice

**Where.** `traversal.glsl:267` and `traversal.glsl:564`.

**What.** `committedHit` reads `instanceAt(instance)`, `meshAt(placement.mMesh)`, then
`triangleCorners` and `triangleNormal`. `resolveFor` then reads `instanceAt` again,
`meshAt` again, `triangleCorners` again and `cornerWeights` again.

**Why it costs.** Per hit this repeats one instance row, one mesh row, one block-table
address load and three index loads. Every primary ray, every bounce, every water ray and
every reflection pays it. With F2 applied the instance row is small, but the index
indirection stays.

**Change.** Let `Hit` carry `uvec3 mCorner` and `vec3 mWeight`. `committedHit` already
computes both. `resolveFor` then skips `triangleCorners` and `cornerWeights`. The mesh row
is still needed for `mShape`, and the instance row for `mMaterial` and `mOpacity`, so those
two loads stay once each.

**Risk.** Low. `Hit` grows by five words. Both the ray-query path and the hit-shader path
go through `committedHit`, so one change covers both.

### F7. `gather` builds the sky source three times for one ray

**Where.** `shading.glsl:114` to `:142`, and `lights.glsl:48`.

**What.** The loop calls `skySourceAt(source)` for each of the three sources. After the
pick, `skySourceAt(picked)` runs again. `skyVisible(position, picked, sunDraw)` then calls
`skySourceAt(picked)` a third time. `skySourceAt` holds a branch and a computed index into
the moon array in the uniform block.

**Change.** Keep the winner as you weigh. Hold `pickedCosine`, `pickedWeight` and a
`SkySource pickedSky` in the loop. Give `skyVisible` a `SkySource` parameter instead of an
index. `fogscatter.comp:233` and `:235` can use the same signature.

**Why this helps twice.** It also removes `cosines[]` and `weights[]`, which is F13.

**Risk.** None, if the draw order is unchanged. The pick compares `moonDraw[1].x` against a
running share, and that arithmetic must stay identical.

### F8. `textureSize` is called inside per-sprite and per-cascade loops

**Where.** `sea.glsl:151` in `waveLevel`. `sprites.glsl:102` in `puffLight`.
`fog.glsl:450` in `fogVolumeAlong`. `fogscatter.comp:78`.

**What.** `waveLevel` asks the driver for the grid size of a wave tile. That size is
`sWaveTiles[cascade].mGrid`, which is 512 and 128, and both are compile-time constants.
`waterSurfaceAt` and `caustic` each call `waveLevel` once per cascade. `waterColumn` calls
`caustic` up to four times per pixel, and `lightThroughWater` calls it again.

**Why it costs.** An underwater pixel makes about twelve texture-header reads for two
constants. `puffLight` runs once per covering sprite and queries a 3D texture size each
time.

**Change.** Send the texel size of each wave tile in `VisibilityConstants`, beside
`mWaveExtent`. The pass already writes `mWaveExtent` from the tile that made it, so the
same line can write the texel size. For `puffLight` and `fogVolumeAlong`, hoist the query
to the caller, or send the froxel grid size in the frame block. `FogVolume` knows
`mColumns` and `mRows` on the host.

**Risk.** None. The values are already known on the host.

### F9. `fogColumn` recomputes a ray-constant exponential for every sprite

**Where.** `sprites.glsl:557`, which calls `fog.glsl:506`.

**What.** `fogColumn(origin, direction, span)` computes `from = origin.z - base`, which
does not depend on `span`. In the usual case both `from` and `to` are above the base, so
`enters` is `from` and `entering` is `exp(-from / scale)`. That exponential is the same for
every sprite along one ray.

**Why it costs.** A pixel in heavy rain walks tens of sprites. Each one pays two `exp`
calls where one would do. `mediumAlong:167` makes the same call once, so the cost is
concentrated in the sprite march.

**Change.** Split `fogColumn` into a per-ray part and a per-distance part. Return a small
struct from the per-ray part and hand it to the per-distance part. `spritesAlong` then
builds the per-ray part once, before the sprite loop.

**Risk.** Low. Keep the three-case shape exactly, because a ray can start below the base.

### F10. The peel loop integrates the whole fog column once per layer

**Where.** `visibility.rgen:202` to `:232`, and `:260` to `:288`.

**What.** `mediumAhead` runs once per peeled layer and once after the loop. Each call
integrates the air from the eye to the current distance. The loop then subtracts
`peeledScattered` to recover the increment.

**Why it costs.** `PEEL_LAYERS` is 4, so a pane over a pane over a pane costs five full
column reads. Each read is about five 3D texture fetches plus one image load. A sixth call
follows for `airBeforeLayer`. The subtraction of two near-equal integrals also loses
precision where the two layers are close together.

**Change.** Integrate forward. Keep the running transmittance and the running scatter, and
advance them from the previous distance to the new one. `fogThrough` in `froxel.glsl:112`
already has exactly this shape, and `fogVolumeAlong` already reads a prefix from
`fogVolumeAir`. The increment is one slice read plus the partial step.

**Risk.** Medium. The arithmetic changes, so the picture moves. This is the change most
likely to improve the image as well as the cost, because the differencing goes away.

### F16. `paintedOver` calls `pow` to compute an identity

**Where.** `sprites.glsl:126` and `:132`.

**What.** `paintedOver(painted, crossings)` returns
`1 - pow(1 - min(painted, SPRITE_ALPHA_LIMIT), crossings)`. An oriented sprite always sets
`fraction` to `1.0` at `sprites.glsl:493`. For that case the result is
`min(painted, SPRITE_ALPHA_LIMIT)`.

**Why it costs.** Every streak sprite, which is every raindrop, pays a `log2` and an
`exp2` for a `min`. The additive branch at `:574` calls the `vec3` overload, which is three
of each.

**Change.** Test `crossings` against one and return the base where it matches. The test is
uniform across the whole emitter run, so the branch is coherent.

**Risk.** None. The two forms agree exactly at `crossings == 1.0`.

### F17. `pow` pairs share a logarithm that is taken twice

**Where.** `sprites.glsl:634` and `:635`.

**What.** The code computes `pow(layer, sprite.mSunLayers)` and
`pow(layer, sprite.mSkyLayers)`. Both take the logarithm of the same `layer`.

**Change.** Take `log2(layer)` once. Then use `exp2` twice. This saves one transcendental
per non-additive billboard.

**Risk.** None at the accuracy these values carry. Confirm with the frame hash.

### F28. `resolveFor` fetches the diffuse texel twice for a pane

**Where.** `traversal.glsl:664` and `traversal.glsl:677`.

**What.** `sampleAlbedo` fetches a `vec4` and returns the `rgb`. Where the surface is
see-through, `sampledOpacity` fetches the same texel at the same level again for the `a`.
`coneLod` runs a second time too, with its own `textureSize`.

**What the comment says.** The comment above line 675 states that an out-parameter "costs
every opaque surface in the frame". The claim is about register pressure, not about a
fetch, and an unused out-parameter should vanish at inline time.

**Change.** Make the helper return the `vec4`. Let the caller take `.rgb` and `.a`. Keep
`sampleAlbedo` as the name for the delighted colour and add one line for the alpha.

**Risk.** The original claim came from a measurement. Repeat that measurement before you
keep the change. Take it at a place with glass, such as a Balmora interior.

---

## C. Branches and divergence

### F12. `tangentTo` and `squareTo` branch where a branch-free basis exists

**Where.** `random.glsl:203` and `spriteshade.comp:77`.

**What.** Both build a tangent from an axis. Both pick a helper vector with a branch, then
take a cross product, then normalize. The two differ in the threshold and in the order of
the cross, so they are near-duplicates rather than one function.

**Why it costs.** `tangentTo` runs inside `coneDirection` and inside `cosineDirection`.
Those run for every shadow ray, every bounce and every ambient ray. Each call spends one
branch, one cross and one reciprocal square root.

**Change.** Use the branch-free orthonormal basis of Duff and others, "Building an
Orthonormal Basis, Revisited", Journal of Computer Graphics Techniques 6(1), 2017. It
produces both tangents from the sign of `z` with no branch, no cross and no normalize.
Frisvad's earlier form has the same shape with a pole problem that the 2017 paper fixes.

**Change, second part.** `coneDirection` and `cosineDirection` then share one helper that
maps a local direction onto the axis. The two functions differ only in how they draw the
local direction.

**Risk.** The generated directions change, so every stochastic term moves. The estimator
stays unbiased. Compare over ten pairs and read the radiance channel as floats.

### F13. Small arrays with a computed index can spill to local memory

**Where.** `shading.glsl:111` to `:142`, `cosines[]` and `weights[]`. `fog.glsl:340`,
which returns `vec3[SKY_SOURCES]`. `fog.glsl:263`, `mMoons[2]` inside `FogSources`.

**What.** These arrays are indexed by a value the compiler cannot fold, such as `picked`
or `sources.mMoon`. A GLSL local array with a computed index usually lands in scratch
memory on NVIDIA hardware.

**Why it costs.** A scratch access is a memory operation in the hottest shader in the
frame. `SKY_SOURCES` is three, so the array is small, but the traffic is real and the
register allocator loses the values.

**Change.** For `gather`, F7 removes the arrays outright. For `moonsInAir` at
`fog.glsl:319`, select between the two moons with a `mix` on a float weight instead of an
index. For `fogSourceTermsAlong`, return a named struct with three members instead of an
array.

**Risk.** Low. Check the SPIR-V for `OpVariable` in the `Function` storage class with an
array type, which is the signature of a spill.

### F25. The candidate walk exists in two forms

**Where.** The `RTX_RESOLVE` macro at `traversal.glsl:190`, and the hand-written loop at
`medium.glsl:94` to `:154`.

**What.** GLSL forbids a `rayQueryEXT` parameter, so the macro is the right answer for the
shared part. `mediumAlong` does not use it. It repeats the candidate header, the vertex
fetch and the world-space cross product, then does its own work.

**Why this matters.** The two walks must agree about what a candidate is. They already
differ: `mediumAlong` ignores `rayQueryGetIntersectionTypeEXT` results the same way, but it
takes no `seeThrough` flag and confirms nothing.

**Change.** Split the shared header into a macro that names the six candidate values. Let
both `RTX_RESOLVE` and `mediumAlong` open with it. The bodies then stay separate, which is
correct, and the header cannot drift.

**Risk.** None. This is a text move.

---

## D. Symmetry and duplication

### F21. Sixteen headers repeat the same host alias block

**Where.** Every file in `components/rtx/shaders/` except `portable.h`, `look.h`,
`colour.h` and `tone.h`.

**What.** Sixteen headers contain this block, with the alias list trimmed each time:

```cpp
#ifdef RTX_HOST

#include <cstdint>

#include <osg/Vec2f>
#include <osg/Vec3f>

namespace Rtx::Shaders
{
    using vec2 = osg::Vec2f;
    using vec3 = osg::Vec3f;
    using uint = std::uint32_t;

#endif
```

Nineteen headers open `namespace Rtx::Shaders` by hand and close it by hand.

**Change.** Put the aliases in one header, such as `hosttypes.h`, beside `portable.h`.
Include it from `portable.h`. Each header then opens the namespace and nothing else. The
`#ifdef RTX_HOST` fences for the namespace stay, because GLSL has no namespace.

**Risk.** None. This is the same trade the tree already made for `NO_TEXTURE`, which the
comment in `scene.h:50` describes.

### F22. Five headers define and undefine a `uint64` macro

**Where.** `probe.h`, `skinning.h`, `spritebin.h`, `spriteshade.h`, `scene.h`.

**What.** Each does `#define uint64 uint64_t` for GLSL and `#undef uint64` at the end.
`spritebin.h` includes `scene.h`. `scene.h` undefines the macro before `spritebin.h`
defines it again. The order works today by accident of the include graph.

**Change.** Define it once in `portable.h` and never undefine it. A macro that names a
type is safe to leave in scope for one translation unit. Remove the five pairs.

**Risk.** None. `glslc` compiles one translation unit.

### F23. Each compute shader spells the bounds test differently

**Where.** Eleven compute shaders.

**What.** The same test appears in at least four forms:

```glsl
if (at.x >= int(frame.mCamera.mWidth) || at.y >= int(frame.mCamera.mHeight)) return;
if (pixel.x >= frame.mWidth || pixel.y >= frame.mHeight) return;
if (any(greaterThanEqual(column, grid))) return;
if (cell.x >= count || cell.y >= count) return;
```

**Change.** Add one helper to a shared library file, such as
`bool outsideOf(uvec2 pixel, uvec2 extent)`. Use it everywhere. The `ivec` and `uvec`
mixture then goes away as well.

**Risk.** None.

### F24. `tone.h` pulls the whole scene description for one struct

**Where.** `components/rtx/shaders/tone.h:11`.

**What.** `tone.h` includes `visibility.h` for `Camera` and `StarField`. `visibility.h` is
707 lines and brings `scene.h`, `wave.h`, the ten weather constants, `CloudDeck`,
`SkyPatch`, `MoonDisc` and the 1112-byte `VisibilityConstants`. `fogvolume.h` does the
same for the same reason.

**Why this matters.** `tone.comp` declares
`GL_EXT_shader_explicit_arithmetic_types_int64` only because `scene.h` demands it. The
shader uses no 64-bit value.

**Change.** Move `StarField`, `SkyPatch`, `CloudDeck`, `MoonDisc`, `SkySource` and the
weather constants into a `sky.h`. `visibility.h` then includes it, and `tone.h` includes
`sky.h` and `camera.h` alone.

**Risk.** None. This is a header split.

### F29. Wave pass and exposure pass declare bindings by hand

**Where.** `wavepass.cpp`, `sFormBindings`, `sLineBindings`, `sComposeBindings`.

**What.** These spell `VkDescriptorSetLayoutBinding{ n, TYPE, 1, VK_SHADER_STAGE_COMPUTE_BIT }`
by hand. `dispatch.hpp` holds `computeBinding` and `computeBindings<N>` for exactly this.
`tonepass.cpp` and `exposurepass.cpp` use `computeBinding`, so the tree already has two
styles.

**Change.** Use `computeBinding` everywhere. `sFormBindings` becomes
`computeBindings<3>(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)`.

**Risk.** None.

---

## E. Host binding code

### F11. Fifteen hit groups exist where three and a payload word would do

**Where.** `visibilitypass.cpp:68` for `sHitRecords`, `visibility.h:55` for `HitRecord`,
`hitstage.glsl:26` for `HitRecordBlock`, `scene.h:369` for `HIT_RECORD_LAYERS`.

**What.** Each closest-hit shader is placed in the shader binding table five times. The
five copies differ only in a `uint mLayer` in the shader record. The ray generation shader
passes the layer as the shader binding table offset at `visibility.rgen:116`.

**Why this is more than it needs.** The payload already travels from the ray generation
shader to the closest-hit shader. One more `uint` in `VisibilityPayload` carries the layer
directly. The record, the record table, the `HIT_RECORD_LAYERS` constant, the
`mHitRecordsPerShader` plumbing in `tracepipeline.cpp` and the per-hit shader-record read
all go away.

**Change.** Add `uint mLayer` to `VisibilityPayload`. Set it in the ray generation shader
before each trace. Read it at the top of each closest-hit shader, before `clearAnswer`.
Pass `0` as the shader binding table offset. `TraceShaders` then drops two fields.

**Why be careful.** The payload sets the register budget for the whole pipeline. One word
against a 27-word payload is small, but measure it. The shader binding table shrinks from
19 entries to 7, which helps nothing on its own.

**Risk.** Medium. The gain is simplicity, not speed. Take it for the simplicity and check
that the trace time does not move.

### F19. `wavecompose` declares a fourth binding that nothing uses

**Where.** `components/rtxvulkan/wavepass.cpp`, `sComposeBindings`.

**What.** The array declares four bindings. `wavecompose.comp` declares three, at 0, 1 and
2. `WavePass::record` pushes three writes. Binding 3 is a storage image that no shader
names and no code writes.

**Change.** Delete the fourth entry.

**Risk.** None. This is a defect, not a trade.

### F20. Four library files are absent from `RTX_SHADER_LIB`

**Where.** `components/rtxvulkan/CMakeLists.txt`, the `RTX_SHADER_LIB` list.

**What.** The directory holds 24 files under `shaders/lib/`. The list names 20. It omits
`froxel.glsl`, `medium.glsl`, `starfield.glsl` and `texturearray.glsl`.

**Why this matters.** The comment over the list says the names exist so that an IDE lists
them beside the shaders. The build itself is correct, because `glslc -MD` finds the real
graph. Four files are invisible in an IDE.

**Change.** Add the four names.

**Risk.** None.

### F26. The bin passes use shared words where a subgroup ballot fits

**Where.** `spriteruns.comp:99` to `:142`. `spritestarts.comp:62` to `:68`.

**What.** `spriteruns.comp` sets a bit in a shared word with `atomicOr`, then reads the
word after a barrier and counts the bits below its own lane. That is `subgroupBallot` and
`subgroupBallotExclusiveBitCount`. `spritestarts.comp` runs a Hillis-Steele scan over 1024
lanes with two barriers per step, which is 20 barriers.

**What the comment says.** `spriteruns.comp:22` states that a subgroup ballot "is the same
thing with a device requirement attached". The tree also records at `bindings.glsl:137`
that a subgroup reduction for the hit counter measured no better.

**Why the two cases differ.** The hit-counter measurement was about one atomic on 5.5 per
cent of rays. This is about a barrier per 32 sprites across every tile, and about 20
barriers in the scan. The requirement is `VK_KHR_shader_subgroup_ballot`, which is Vulkan
1.1 core and present on every card at or above the Turing floor this fork states.
`SPRITE_RUNS_LANES` is 32, which is the subgroup size the device reports.

**Change.** Measure a ballot version of `spriteruns.comp` and a `subgroupExclusiveAdd`
version of `spritestarts.comp`. Keep whichever wins. Record the number either way.

**Risk.** Low. Both are exact replacements with the same result.

### F27. `spriteshade` sorts in device memory and reads with one lane

**Where.** `spriteshade.comp:179` for `sortRun`, and `:280` to `:308` for the main loop.

**What.** `sortRun` is a bitonic sort over the `Order` buffer, which lives in device
memory. Every compare-exchange reads and writes device memory, and each stage ends with
`groupMemoryBarrier` and `barrier`. The main loop then runs one step per sprite. In each
step lane 0 alone reads the grid and writes the result, while 1023 lanes wait at a barrier.

**Why it costs.** The sort is `O(n log^2 n)` round trips to device memory. The main loop
holds two barriers per sprite, and one of the two phases uses a single lane.

**Change.** Sort in shared memory where `held.mCount` fits the workgroup. The workgroup is
1024 threads and a sprite key is 8 bytes, so 8 kB of shared memory covers 1024 sprites.
Fall back to the device-memory sort above that. For the main loop, batch the read: let 32
lanes read 32 sprites' `readAt` values into shared memory, then scatter the writes.

**Risk.** Medium. The accumulation order into `grid[]` decides the answer, so the batch
must keep the front-to-back order. Measure at Balmora at night in the rain, which is where
this pass is loaded.

---

## F. Notes on what is already right

These are recorded so that a later reader does not undo them.

- `precise` on the ray derivation in `camera.h:157`. The comment gives the exact failure
  and the exact cause. Keep it.
- The `RTX_RESOLVE` macro. GLSL forbids a `rayQueryEXT` parameter, so a macro is the only
  shared form.
- The unused draws in `gather` at `shading.glsl:78`. They hold the position of the lamp
  draw in the sequence. Do not delete them. A named `skipDraws(state, 3)` states the intent
  better than a dead array, and also removes the array of F13.
- The atomic hit counter. The subgroup alternative was measured and lost.
- The three-channel `CHANNEL_TRANSPARENCY_OPACITY`. The one-channel form tinted the frame
  cyan, and the comment records it.
- `writeonly` on every set 2 image. This is the right qualifier and it helps the compiler.
- The blocked vertex buffers and the address-in-uniform table design. Both were measured
  and both are documented with the number they cost.
- `VisibilityPass::pushInputs` appends writes with a cursor and asserts the count. That is
  the shape that prevents the class of mistake the comment describes.

## G. What was verified in the compiled modules

These claims were checked against the SPIR-V in `build-debug/resources/rtx/shaders/`, not
against the source alone. The command is
`spirv-dis --no-color <module>.spv`.

- **The optimizer narrows a struct load.** `visibility.rahit.spv` loads `GpuInstance`
  members 0 and 1 with two four-byte loads, and never touches `mMotion`. F2 was reworded
  for this.
- **Constant zero levels are common.** `visibilitysurface.rchit.spv` holds 39 explicit-level
  samples. 29 of them take a literal zero. Some are correct by design, such as the shading
  map and the cloud sheet. The shadow path's cutout test is among them, which is F1.
- **Ten size queries survive per hit shader.** `visibilitysurface.rchit.spv` holds ten
  `OpImageQuerySizeLod` instructions. This supports F8.
- **`glslc` is run with `-O` outside a Debug build.** `CMakeLists.txt` sets
  `$<IF:$<CONFIG:Debug>,-O0,-O>`. A claim about generated code must therefore name which
  build directory it was read from.

What was **not** verified: whether the duplicate triangle resolution of F6 and the second
diffuse fetch of F28 survive the optimizer. The line tables show both source lines, but
the counts include every inlined call site and do not separate the entry path. Read the
disassembly around the entry block before you accept either number.

---

## Suggested order of work

1. F19, F20. Two defects, no risk, no measurement.
2. F21, F22, F23, F24, F29. Header and style unification, no behaviour change.
3. F14, F8, F16, F17. Cheap wins with no change to the picture.
4. F6. One fewer triangle resolution per hit, no change to the picture.
5. F1. One behaviour change with a clear reason. Measure over ten pairs.
6. F7, F13, F25. Consolidation of the sky sources and the candidate header.
7. F3, F4, F18. Shared-memory tiles for the two filter passes. Measure each alone.
8. F5. Format narrowing. Take the reading with `--exposure=1`.
9. F9, F10. The fog column. F10 changes the picture and should improve it.
10. F2, F11, F12, F26, F27. The larger changes. Measure each and record the number.

Run the verification chain after each step, and run
`apps/rtxtool/repeatable.sh --pairs=10` before you call any of them done.
