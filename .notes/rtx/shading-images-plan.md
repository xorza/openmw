# Shading maps and terrain masks as images

Item 6 of `shader-review.md`: `paintedLight` and `maskWeight` read small grids out of buffers of
floats by hand, four loads and the wrap or the clamp apiece, on every albedo read at every hit and
every ground layer. The proposal is one bilinear fetch each, from an image the texture unit filters.

**Not started.** This file is the route: what the read costs, what to build, in what order, what
proves each step, and what could go wrong.

## 1. What the read costs

The review's rule is to take the number before the machinery. The harness already has the switch
for half of it: `--delight=0` makes `sampleAlbedo` return before `paintedLight`, on a uniform branch,
so the two legs differ in the shading read and in nothing the trace does.

`shot --upscale=off --repeat=32`, GPU `trace` zone medians, two rounds interleaved on a warm card:

| view                 | round | delight on | delight off |
|----------------------|------:|-----------:|------------:|
| balmora              |     1 |      2.99 |       2.47 |
| balmora              |     2 |      3.08 |       2.75 |
| seyda-neen-ship-dawn |     1 |      6.37 |       5.12 |
| seyda-neen-ship-dawn |     2 |      4.31 |       5.74 |

Balmora says a third to a half of a millisecond out of three. The dawn view moved by more between
rounds than between legs, so it says nothing, and the reason is the one `CLAUDE.md` gives: the
clock and the temperature the harness prints beside each result were not held. That is the first
step below, done properly.

**Why it could be that much.** The table is 2.4 MB of floats for 586 textures, read at four places
per albedo, at an address the hit decides. Nothing about that access is cached the way a texture
fetch is, and a bounce hit lands anywhere in it. The mask read is the same shape per ground layer,
four or five layers a hit, on a buffer that grows and shrinks with the chunks.

## 2. What to build

### 2.1 Shading maps: a second bindless array beside `textures[]`

One `r16_unorm` image of `SHADING_EXTENT` squared per texture slot, in a second binding of set 1 with
the same count, the same `PARTIALLY_BOUND | VARIABLE_DESCRIPTOR_COUNT` flags and the same slot index.
`paintedLight` becomes one `textureLod` at level nought through the array's own repeat sampler, which
is the wrap the function does by hand today.

**A bindless array and not a `sampler2DArray`**, for two reasons the review's second option runs
into. `maxImageArrayLayers` is 2048 on this device and `sMaxTextures` is 4096. And an array image
grows only by being rebuilt, where a descriptor array takes one write per arrival — which is the
rule the texture array already lives by, and the reason a texture arriving is not a frame spike.

**Sixteen bits and not eight.** The map is a divisor between `ShadingMap::sFloor` and `sCeiling`,
half and two. Stored as `value / sCeiling` in eight bits, a step is 0.8 per cent of the range and
1.6 per cent of the value at the floor, and that is a band on every wall the estimate brightens. In
sixteen bits a step is a part in thirty thousand. The image is 2 KB a texture and 8 MB at the array's
maximum, half of the table it replaces rather than the review's quarter. The scale lives in `scene.h`
so the encode on the host and the decode in the shader read one number.

**The host's `paintedLight` stays.** The composite bake and the contact sheet read the map on the CPU
and must keep reading the same map: the swap-over between a flattened chunk and its live stack is a
comparison between the two. The sampler filters linearly in the stored value, as the host does, so
the two differ by the encoding step and by the eight-bit weights the texture unit interpolates with,
and nothing else.

**A slot with a texture always has a map**, neutral where nothing could estimate one, exactly as the
table holds ones there today. A descriptor that is bound and never written is undefined to read, and
`Texture.cpp` says what that cost once.

### 2.2 Masks: a bindless array of their own, with a clamping sampler

One `r8_unorm` image per layer that has a mask, in an array of its own with a sampler that clamps.
`maskWeight` becomes one fetch at `uv * transform.xy + transform.zw`, and the half-texel and the clamp
it does by hand are the sampler's.

**Eight bits is exact here.** The blendmap the terrain hands over is `GL_ALPHA, GL_UNSIGNED_BYTE`,
`esmterrain/storage.cpp` line 421, so the image holds the file's own bytes and the float the
extractor reads today is a conversion the GPU path no longer needs. The host keeps the floats for the
composite bake, or the bake reads the bytes; either is one source.

**Its own array and not a tile in the shading array**, because a mask's edge is its own last texel
and a clamping sampler answers that at any size — the blendmap of a chunk of one cell is seventeen
across, `quadCount * quadSize + 1`, and every chunk past a cell is flattened and carries no mask. A
tile padded into a larger image would clamp at the tile's edge and not the mask's, and would need the
coordinate clamped by hand again.

**Slots and not offsets.** `SceneDesc::addMask` hands out a slot from a free list where it hands out
a run of the float buffer today, and `GpuLayer` carries the slot and the transform and drops the
offset, width and height. The chunk churn that `mMaskRuns` handles becomes the image churn the
texture array already handles: create on arrival, bury on release, one descriptor write each.

### 2.3 One array class, three uses

`TextureArray` is already a bindless array of combined image samplers with a partially bound tail,
a variable count, per-slot arrival and burial. The shading array is the same thing at a second
binding of the same set, indexed by the same slot; the mask array is the same thing again in a set of
its own with a different format and sampler. What to factor is the descriptor machinery — the
layout, the pool, the variable count, the per-slot write — and not the upload, which differs.
`laterSets(textureLayout)` gains the mask set, and every pipeline that resolves a hit binds it.

## 3. In what order

Each step leaves the tree building, its tests passing and the picture unchanged, and is measured
before the next.

1. **The number.** `bench --suite=exteriors --window=false` with `--delight=1` and `0`, legs
   interleaved, three alternations, the clock and temperature columns checked. Then the same for
   the masks with a shader hack that keeps the branch — replace the four mask loads by the first —
   so what is removed is loads and not layers. If both together are under a tenth of a millisecond
   the item closes here with the numbers written into `shader-review.md`.
2. **The shading array.** `TextureArray` gains the second binding, a `ShadingImage` per slot made
   beside its `Texture`, buried beside it. `mShadingValues`, `mShading`, `growShading`, `reshade`,
   `gatherShadingAt` and `BIND_SHADING` go. `paintedLight` in `texturing.glsl` becomes the fetch and
   the encode constant lands in `scene.h` beside `SHADING_EXTENT`.
3. **The mask array.** The factored array with `r8_unorm` and a clamping sampler, its own set and
   layout, `addMask` handing out slots, `GpuLayer` shrunk, `scenebuffers.cpp` no longer uploading
   masks, `BIND_MASKS` gone, `maskWeight` the fetch.
4. **The number again**, the same protocol, and the review item marked done with both figures.

## 4. What proves it

- **Unit, `shadingmap.cpp`:** the encode and decode round trip inside one step of the format, and
  the neutral map encodes to exactly the value the shader decodes to one.
- **GPU, `visibility/surfaces.cpp`:** the test that sets `mDelight` on a wall gains a wall whose map
  is a known gradient, and asserts the pixel against the host's `paintedLight` at the same
  coordinate within the encoding step and the sampler's weight precision. Two coordinates, one on a
  cell centre and one between cells, so the bilinear and the wrap are both in the assertion.
- **GPU, `groundSumsItsLayersByTheWeightsItsMasksName`:** a mask that is not square and not centred
  on the chunk, read at a point that lands between texels and at a point past the mask's edge, so the
  half-texel convention and the clamp are both pinned to the same numbers the old read gave.
- **The swap-over:** a flattened chunk and its live stack agree within the encoding step where the
  two meet, which is the composite tests' claim carried onto the device — a new test if none reads
  a composite on the GPU today.
- **`verify --views=all`** before and after each step: the picture may move by the encoding step
  and by nothing else, and the interiors not at all.

## 5. What could go wrong

- **Descriptor count.** Two arrays of 4096 combined image samplers per pipeline, and a third for the
  masks. The limit here is a million, and `Texture.cpp` says why the array is declared at its
  maximum and allocated at the scene's.
- **Tiny images.** One 2 KB image per texture and one per mask layer, each an allocation, each a
  descriptor write. `maxMemoryAllocationCount` is unlimited on this driver, and the texture array
  already makes one image per texture on the same path. What to watch is the arrival cost in
  `bench --suite=streaming`, where a cell's textures land in one frame.
- **`nonuniformEXT`.** Every index into the new arrays is a hit's, so every read carries it, as the
  texture reads do.
- **Precision at the swap-over.** The composite is baked from floats and the live stack reads
  sixteen-bit maps; the difference is bounded by the step and the test in §4 pins it. If it shows,
  the bake reads the encoded map, which is one more decode and no second source.
- **The neutral map.** A texture that would not load gets the default `ShadingMap`, ones
  everywhere, and that image has to be uploaded like any other — a slot with a texture and no map
  is the bug the array's comments already describe.

## 6. Out of scope

- The estimate itself, its floor and ceiling, and the extent of the grid.
- The composite path and where `sCompositeFrom` puts the swap-over.
- Reading the mask through a mip chain: a mask is read at one level, because a chunk that is far
  enough to want a coarser one is flattened.
