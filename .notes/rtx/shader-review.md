# RTX shader review

A pass over every file under `components/rtx/shaders/` and `components/rtxvulkan/shaders/`,
against what current ray-traced renderers on Ada-class hardware do, and against the tree's own
posture: a clean seam, one answer stated once, allocation and bandwidth as metrics, the strongest
technique over the safest. What was small and safe was applied and is listed first. What changes
the shape of a pass or a table is written up below it, ranked by what it is worth, each with the
evidence, the proposal, the risk and how to measure it.

## Applied

Every view `openmw-rtxtool verify` renders came back byte-identical, and the 508 tests under `Rtx*`
pass.

- **`lib/frame.glsl`**: the frame's predicates asked one way. `sunUp()`, `skyLights()` and
  `waterOver(position)` replace five spellings of the sun test, six of the ambient test and six
  subtractions against the water level across the shading, the fog, the sky, the water and the
  sprites. Each pairs the specialization constant with the runtime test it stands in front of.
- **`TexturePoint`** in `texturing.glsl`: a hit's place on a sheet and its triangle's corners there,
  built once per transform. The albedo, the opacity and the emissive map used to each transform the
  three corners again, and the de-lighting a fourth time behind a comment saying why it could not be
  handed back.
- **One block resolution per triangle** in `geometry.glsl`: the index, texture-coordinate and normal
  block of a hit are each looked up once, not once per corner. A mesh's run never straddles a block,
  which `SceneDesc::addMesh` enforces, so the three corners share one.
- **The light grid rides in the frame block.** `GpuLightGrid` is a field of `VisibilityConstants`,
  filled by `VisibilityPass::record` the way the sea's tables are. That removes a descriptor, a
  buffer per frame slot, and a storage read at every lamp lookup where the constant bank serves.
  Set 0 declares 23 bindings, from 24.
- **`GpuMaterial::mDiffuseColour` is a `vec3`**: its alpha was a second copy of `mOpacity` that the
  shader never read. Sixty-eight bytes a row, from seventy-two.
- **Sprites read their mip from `coneAt`**, not from `mSpreadAngle`. Under the parallel projection a
  map tile traces with, the angle is nought and every sprite read level zero; the cone's width is
  the answer there, exactly as it is for textures and the wavelet. Perspective frames are unchanged.

## What it cost, and why

**The exterior trace is a tenth of a millisecond slower, and the cause is the driver's register
allocation rather than any of the work.** Measured over three alternating runs of the harness bench
against snapshots of both builds, the `trace` zone went from 1.56 to 1.67 ms at the ship at Seyda
Neen and from 1.99 to 2.15 at dawn; the interior was 1.57 either way, the `air` zone was unchanged,
and the game's own bench (`bench.sh`, three saves) moved within its run-to-run noise. The SPIR-V has
4 percent fewer instructions and 9 percent fewer loads.

The pipeline statistics the harness logs at every pipeline it builds say what happened: the
exterior kernels compiled at 128 registers before and at 96 registers with more spilling after, and
the interior kernel was at 96 already. The driver's occupancy heuristic sits on a knife-edge for
these kernels. Reverting each edit alone from the full set left 96; adding each edit alone to the
old shaders found that the block lookup, the texture point in either of two forms, and the rest of
the set taken together each tip it on their own. There is no Vulkan knob for it. A 16 by 16
workgroup does bring every kernel back to 128 registers, because at 256 threads the smaller count
buys no occupancy, and it measured worse still — 1.81 and 2.57 ms — as the spill area per
workgroup grows four times.

**So the shape is kept and the cost is recorded.** Putting the block lookup back to one per corner
was measured too, at 1.73 and 2.22 ms: within the 96-register regime the fewer loads are worth
having. The lesson for the next change to the trace is to read `Register Count` in the log before
reading the timer. The durable fix is register pressure itself — the ray generation shader of
finding 1, and the narrower G-buffer of finding 4 — and until then a kernel this close to the edge
will keep flipping on edits that change nothing about the work.

## Design findings

## Which unit each pass is waiting on

**Measured rather than argued, and it retires most of what is below.** Nsight Systems samples the
device's throughput counters at 20 kHz without root on this box —
`nsys profile --gpu-metrics-devices=0 --gpu-metrics-set=ad10x-gfxt --gpu-metrics-frequency=20000` —
and exporting the capture to SQLite gives one row per counter per sample. Bucketing those samples by
ray-core throughput separates the trace from everything else without needing a label on either.

| samples where | SM | RTCORE | L1 | L2 | VRAM |
|---|---|---|---|---|---|
| the ray cores are busy — the trace | 37 | **60–97** | 19 | 19 | 12 |
| they are idle — every compute pass | 43 | 0 | 31 | 23 | 40 |

**The trace is ray-core bound and nothing else.** Its VRAM throughput is twelve per cent at both
1280×720 and 1920×1080, which is the same tenth of peak the byte arithmetic gives. No format, no
packing and no table narrowed will move it — what will is fewer rays and cheaper traversal, which is
finding 2 and nothing on this list.

**One compute pass runs at the memory limit and it is the accumulator.** Eight thousand of the
thirty-nine thousand compute-dominated samples sit at 80–100% VRAM with the SMs at twelve per cent,
which is 0.44 ms a frame against a measured `accumulate` of 0.42. Finding 4 has already taken it
from 0.685.

**And the cascade is bound by the work per tap rather than by bytes**, which §4 measured directly:
ten instructions a tap and eight bytes a tap cost the same there.

So the ranking below is about the ray core first, the host second, and memory traffic last — the
opposite of the order the findings were written in.

### 1. Shader Execution Reordering needs a ray generation shader, and the trace is a compute kernel

`visibility.comp` traces with `rayQueryEXT` from a compute dispatch, and `VISIBILITY_STRIP` is a
hand-written coherence permutation over workgroups. `lib/variants.glsl` says the trace is
occupancy-bound and measures half a millisecond for the registers one unused path costs — the same
divergence SER exists to sort: a warp holds sky pixels, water pixels, terrain pixels with a five-layer
stack, panes that trace twice, and a fog path that is a closed form on one frame and a volume read
on another.

`VK_NV_ray_tracing_invocation_reorder` (`reorderThreadNV`) is only legal in a ray generation shader.
The change is not a move to closest-hit shading: the kernel body goes into a `.rgen` as it stands,
still tracing with ray queries, and calls `reorderThreadNV(hint, bits)` once after the primary
traversal with a hint of what the pixel is about to do — miss, water, terrain, surface, pane, and
under-water — before shading. That is the form NVIDIA's own samples use for inline traversal with
SER. What changes on the host is a ray tracing pipeline with a one-entry shader binding table and
`vkCmdTraceRaysKHR` in place of the dispatch; the descriptor sets, the specialization constants and
the frame block stay. `VISIBILITY_STRIP` goes, since the launch order becomes the reorder's.

The specialization variants that take the moons and the sea out of a room stay worthwhile; SER
sorts divergence, it does not remove register pressure. `ser-plan.md` beside this file is the
investigation and the staged plan.

Measure with `bench` over the suite, p99 and worst frame beside the median, and `shot` at the ship
at Seyda Neen and the mages' guild first.

### 2. Opacity micromaps for every cutout, baked at load

`candidateStops` reads a texture for every candidate on every non-opaque instance, for every ray —
the eye's, the bounce, the ambient ray, the sun and lamp shadow rays, the water's two, and the fog
volume's eight probes per column. A canopy over a shadow ray is a texture fetch per leaf card the
ray crosses. This is the largest "preprocess once at load" item in the tree.

`VK_EXT_opacity_micromap` is exactly this: the alpha mask of each triangle, resolved against the
material's cutoff, baked into a micromap when the mesh arrives and attached to the bottom-level
build. Traversal then resolves fully opaque and fully transparent microtriangles in hardware, and
only the *unknown* state reaches the candidate loop — which keeps `candidateStops` for panes, fades
and the unknown microtriangles and removes it everywhere else. The bake is a per-mesh pass over
`(uv, texture, cutoff)` and can run on the host at arrival beside `ShapeFold`. `Rtx::Material::
isCutout` already decides which instances are non-opaque, so the set that needs a micromap is
already named.

Cost: the bake at arrival, and micromap memory beside each BLAS. Risk: the four-state (`unknown`)
handling must keep the mip argument `candidateStops` makes — a micromap is a level-zero answer, so
choose the two-state format only where the cone argument is not needed, which is the shadow rays.

### 3. Set 0 by device address

Set 0 is 23 bindings, twelve of them storage buffers that are pushed twice a frame (the fog volume
and the trace) as 23 descriptor writes each. `GL_EXT_buffer_reference2` is already required and the
vertex blocks already travel as addresses; `RtxProbeTest` proves the address path on both memory
kinds this renderer uses.

Proposal: a `GpuTables` struct of `uint64_t` addresses — meshes, instances, materials, layers,
masks, lights, light offsets, light indices, sprites, emitters, sprite tile offsets and indices, the
blue-noise tile and the shading maps, plus the three block tables — carried in the frame block (or a
second small uniform, updated when a table is remade). `bindings.glsl` declares one
`buffer_reference` block per table. Set 0 is then the acceleration structure, the hit counter, the
frame block, the two wave sampler arrays and the fog field: six bindings. `pushInputs` shrinks to
those, and the "a binding the layout declares was left unwritten" class of mistake goes with it.

Pair it with merging each offsets-and-indices pair (light grid, sprite tiles) into one buffer with
a header: `LightGrid::rebuild` and `SpriteTiles::rebuild` already rebuild both lists together.

Risk: a device address has no robust-access bounds check where a descriptor does. On NVIDIA the two
paths are the same load unit; this is a debugging-safety trade, not a speed one, and the tests that
address past a table would have to say so through GPU-assisted validation rather than through
robustness.

### 4. G-buffer and history precision — done

**The denoiser is a third cheaper and the picture did not move.** Medians at the guild, release,
1920×1080, against the tree before any of it:

| zone | before | after |
|---|---|---|
| `filter` | 2.72 | 1.92, **−29%** |
| `accumulate` | 0.685 | 0.42, **−39%** |
| `composite` | 0.227 | 0.135, **−41%** |

What moved: `GBUFFER_GUIDE` and `ATROUS_CHANNEL` to half floats, the accumulator's colour and surface
histories with them, and the cascade decoupled from `CHANNEL_INDIRECT` so the two formats could part.
`gbuffer.cpp`, `accumulate.h` and `atrous.h` each carry the argument for their own width and the
measurement behind it.

**The three corrections this finding needed.**

- **The two radiance channels were already closed**, by an experiment `gbuffer.cpp` records: a
  low-discrepancy sampler makes a rounding error alias rather than cancel, so a converged mean came
  back 0.096% low against a 0.067% tolerance. Nothing here reopened it.
- **A world distance does not fit in a half.** `sFarPlane` is 200000 and a half stops at 65504.
  `ACCUMULATE_DISTANCE_RANGE` puts the far plane at 2^15, which clears the overflow and the
  denormals at the near end both.
- **Neither pass runs in the frame the budget is written against.** `upscale = quality` is the
  default and Ray Reconstruction suppresses the wavelet, so every millisecond above belongs to the
  path with no DLSS on it.

**And one thing the finding asked for that does not pay: the packed guide of §9.** Three encodings
were built and benched, guild and ship, three alternations each:

| what one tap holds | decode | `filter`, guild |
|---|---|---|
| a guide load and a depth load | none | 1.92 |
| a half distance in the guide's `w` | one multiply | 1.565 |
| an octahedral normal and an exact distance | fold plus `normalize` | 1.945 |
| three ten-bit components and an exact distance | shifts plus a Newton step | 1.87 |

**The cascade is not bandwidth-bound at eight bytes a tap — it is bound by the work per tap.** The
eighteen per cent the second row buys comes from removing a *load instruction*, and every encoding
that restores the precision costs a decode worth about as much as the load it removed. Ten
instructions a tap and eight bytes a tap price the same here, which is the figure to reach for before
proposing another packing.

**A half float is measurably too coarse for a distance at 1920×1080**: its step is 0.46 of a pixel's
own footprint at every range, because the two scale together. All twenty `verify` views moved, worst
2 to 25 of 255 on up to 14.6% of their pixels. Restoring the distance exactly brought that back to
the 1-to-19 band, and that is the encoding worth 2.6%.

**The filter tests cannot decide a guide's precision.** Both render 64 rows square, where a pixel
subtends seventeen times what it does at 1080p — so every figure they report is identical to five
digits across all three encodings. `verify` at the render resolution is the instrument.

### 5. Vertex normals as octahedral `snorm16x2`

Normals are three floats a vertex, in three copies (`SlotBlocks`), rewritten per frame for every
skinned body. An octahedral encoding is four bytes a vertex: a third of the fetch at every hit and a
third of every skinned rewrite. The decode is a dozen ALU per corner. `TexCoord` stays float —
Morrowind's tiling UVs run to hundreds of repeats and a half loses the texel.

Beside it: `GpuInstance::mMotion` is forty-eight bytes of a sixty-byte row, the identity for nearly
everything. A side table of movers indexed from the row (nought for the identity) makes the row
sixteen bytes and the per-frame `place` write smaller by the same factor. The motion read happens
once a pixel, so the gain is the host write and the table's footprint, not the trace.

### 6. Shading maps as an image

`paintedLight` is four scalar loads with wrap arithmetic on a buffer of `float`s, for every albedo
read at every hit and every terrain layer. A `r8_unorm` (or `r16_unorm`) 32×32 image per texture,
sampled with the array's own repeat sampler, is one bilinear fetch — and a quarter of the memory.
The natural home is a second bindless array beside `textures[]`, indexed by the same slot, or one
`sampler2DArray` of 32×32 layers grown with the texture array. The same argument covers
`maskWeight`, which samples a terrain mask by hand for the clamp: a small `r8_unorm` image with a
clamping sampler does the same in one fetch. `shading-images-plan.md` is the route, with a first
measurement: a third to a half of a millisecond of the trace at Balmora noon.

### 7. The fog volume is one thread per column — **done**

Split into `fogscatter.comp` and `fogintegrate.comp`. The cost was never the reason: the `air` zone
measured 0.25 ms, under this item's own threshold. What the split was worth was the estimator it
allowed — one probe answering for eight slices was most of the noise in a lamp-lit night frame, and
it could not be fixed while a column was one thread. `fogscatter.comp`, `fogdepth.comp`, `FogSlice`
and `Rtx::FogVolume` carry the record.

### 8. Sprite binning on the host

`SpriteTiles::rebuild` and `SpriteShade::shade` run per frame on the host and write the tile lists
to the device, and `SPRITE_TILE` says the tile size was measured against the host write: eight
would take 0.32 ms off the trace and put 2.4 ms on the frame. A bin pass on the device — one
workgroup per emitter, atomic counts per tile, a prefix sum — takes the host out of the loop and
lets the tile be the size the trace wants. `GpuEmitter::mFirst` and `mCount` ride to the device
today and are read only by `SpriteShade`; a device bin is what would read them.

### 9. Smaller things, in order of value

- **A packed guide for the wavelet.** Each tap reads a `rgba32f` guide and a `rg32f` depth; with (4),
  pack the octahedral normal and the distance into one `rgba16f` so a tap is one load. SVGF's
  feedback variant — level one's output as next frame's history — is a free stability gain the
  accumulator's shape already allows.
- **The blue-noise draw** is up to four scalar loads a pixel from a `float` buffer. Read the tile as
  one `vec4` per pixel in `main` and hand the four streams down, or store it as a `rgba16_unorm`
  image.
- **FFT twiddles** are a `sin` and a `cos` per butterfly per stage; a shared-memory table of
  `WAVE_GRID / 2` entries is the standard form. The passes are a fraction of a millisecond, so this
  is tidiness.
- **`GpuMaterial::mKind`** is a word for two bits; with `mLayerCount` naming terrain and `KIND_WATER`
  the only other value, a flag word holding it and `MESH_SHEET`-style bits would shrink the row
  again. Not worth a change on its own.
- **`VisibilityConstants`** is 980 bytes of which the sky patches (264) and the sea's tables (100)
  change on the hour and the weather, not the frame. Splitting them into a second block updated on
  change saves a fraction of a kilobyte a frame: not worth it.

## Reviewed and sound

Kept as they are, with the reason, so the next pass does not re-open them:

- **Specialization variants** over the sun, the moons, the sea and even air: the right tool for a
  kernel whose register count is set by its widest path, and measured.
- **Blue noise across the screen, R2 along time, PCG for the reservoirs**: the field's answer for a
  single-sample-per-pixel path that feeds a spatiotemporal filter.
- **Resampled importance sampling over the lamp list** with the cone-sampled shadow ray: ReSTIR's
  temporal and spatial reuse is the next step and the code has the `Reservoir` shape for it, but
  `lights.glsl` records that perfect selection measured under half a per cent better at two sites.
- **Closed-form fog in a room, a volume out of doors, and the sun's phase applied at the pixel**:
  the sunward image is the standard separation and the `FOG_UNIFORM` variant is the standard
  short-cut.
- **Ray-cone mip selection** with a per-hit `SurfaceCone` and a per-texture `TexturePoint`: the
  Akenine-Möller form, done once per hit as it should be.
- **Reprojection as offsets from the eye**, the camera step differenced on the host: exactly what
  Morrowind's six-figure coordinates need.
- **The wave tiles** as an inverse FFT with the moments composed for LEAN-style lost slope, and the
  caustic as a Jacobian of the same field.
- **Push constants everywhere but the trace**, with the frame block as a uniform buffer: correct,
  and the 0.14 ms for uniform over storage is recorded beside it.
