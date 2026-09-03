# RTX shader review

A pass over every file under `components/rtx/shaders/` and `components/rtxvulkan/shaders/`,
against what current ray-traced renderers on Ada-class hardware do, and against the tree's own
posture: a clean seam, one answer stated once, allocation and bandwidth as metrics, the strongest
technique over the safest. What changes the shape of a pass or a table is written up here, ranked by
what it is worth, each with the evidence, the proposal, the risk and how to measure it. What has been
done is deleted, not marked.

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
finding 1 and nothing on this list.

**One compute pass runs at the memory limit and it is the accumulator.** Eight thousand of the
thirty-nine thousand compute-dominated samples sit at 80–100% VRAM with the SMs at twelve per cent,
which is 0.44 ms a frame against a measured `accumulate` of 0.42. The half-float histories already
took it from 0.685.

**And the cascade is bound by the work per tap rather than by bytes**, which the guide encodings measured directly:
ten instructions a tap and eight bytes a tap cost the same there.

So the ranking below is about the ray core first, the host second, and memory traffic last — the
opposite of the order the findings were written in.

### 1. Opacity micromaps for every cutout, baked at load

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

### 3. Vertex normals as octahedral `snorm16x2`

Normals are three floats a vertex, in three copies (`SlotBlocks`), written by the skinning pass for
every posed body. An octahedral encoding is four bytes a vertex: a third of the fetch at every hit and
a third of what the pass writes. The decode is a dozen ALU per corner. `TexCoord` stays float —
Morrowind's tiling UVs run to hundreds of repeats and a half loses the texel.

Beside it: `GpuInstance::mMotion` is forty-eight bytes of a sixty-byte row, the identity for nearly
everything. A side table of movers indexed from the row (nought for the identity) makes the row
sixteen bytes and the per-frame `place` write smaller by the same factor. The motion read happens
once a pixel, so the gain is the host write and the table's footprint, not the trace.

### 4. Sprite binning on the host

`SpriteTiles::rebuild` and `SpriteShade::shade` run per frame on the host and write the tile lists
to the device, and `SPRITE_TILE` says the tile size was measured against the host write: eight
would take 0.32 ms off the trace and put 2.4 ms on the frame. A bin pass on the device — one
workgroup per emitter, atomic counts per tile, a prefix sum — takes the host out of the loop and
lets the tile be the size the trace wants. `GpuEmitter::mFirst` and `mCount` ride to the device
today and are read only by `SpriteShade`; a device bin is what would read them.

### 5. Smaller things, in order of value

- **SVGF's feedback variant** — level one's output as next frame's history — is a free stability
  gain the accumulator's shape already allows. A packed guide beside it was measured and does not
  pay: the cascade is bound by the work per tap, and every encoding that keeps the distance exact
  costs a decode worth the load it removes.
- **The blue-noise draw** is up to four scalar loads a pixel from a `float` buffer. Read the tile as
  one `vec4` per pixel in `main` and hand the four streams down, or store it as a `rgba16_unorm`
  image.
- **FFT twiddles** are a `sin` and a `cos` per butterfly per stage; a shared-memory table of
  `WAVE_GRID / 2` entries is the standard form. The passes are a fraction of a millisecond, so this
  is tidiness.
- **`GpuMaterial::mKind`** is a word for two bits; with `mLayerCount` naming terrain and `KIND_WATER`
  the only other value, a flag word holding it and `MESH_SHEET`-style bits would shrink the row
  again. Not worth a change on its own.
- **`VisibilityConstants`** is 1096 bytes of which the sky patches (264) and the sea's tables (100)
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
