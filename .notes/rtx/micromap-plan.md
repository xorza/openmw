# Opacity micromaps for every cutout

`micromaps` is a line in `todo.txt` and item 1 of `shader-review.md`. A third of the instances in
a town are cutouts, and every ray that meets one — the eye's, the bounce, the ambient rays, every
shadow ray, the fog volume's probes — stops for a texture fetch to learn whether it landed in a
hole. `VK_EXT_opacity_micromap` is the hardware's answer to exactly this: the mask, resolved per
microtriangle when the mesh arrives, so traversal walks through the holes and commits the leaves
without asking. This plan is the shape `device-address-plan.md` has: what is there, what the field
does, what to build, in what order, what proves it, what could go wrong.

**The device says yes.** `vulkaninfo` on this box: `VK_EXT_opacity_micromap` revision 2,
`micromap = true`, `maxOpacity2StateSubdivisionLevel = 12`, `maxOpacity4StateSubdivisionLevel = 12`,
`micromapHostCommands = false`. The headers are 1.4.357 and `glslc` 2026.3 knows
`GL_EXT_opacity_micromap`. Everything is built on the device, which is where this plan bakes too.

## 1. What is there today

### 1.1 The cutout path

`SceneAcceleration::placeRow` marks a placement `VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR`
where `Material::isCutout` says so — a cutoff and a diffuse texture, and no translucency — and every
ray that meets such an instance runs `candidateStops` in `traversal.glsl`: the instance row, the
material row, the three corners' texture coordinates, a `TexturePoint`, a `SurfaceCone`, and
`sampleDiffuse` at the cone's mip, compared against `mAlphaCutoff`. The eye's ray runs it in
`visibility.rahit`; every ray query runs it inside `RTX_RESOLVE`. `lightThrough` — the sun, the
moons, the lamps, the ambient rays, the sprites, the sea, the fog volume's probes — carries no cone
and reads the finest level, and `traversal.glsl` says why: "aliasing in a leaf's shadow is worth far
less than aliasing on the leaf".

`candidateStops` also says what the cone is for: "A mask point-sampled at its finest mip answers
for one texel out of the hundreds a distant pixel covers, and a binary test on that is a coin toss
per pixel". §3.2 comes back to this, because a micromap is a level-zero answer.

### 1.2 What the content is

`openmw-rtxtool scene --view=balmora`: 8679 instances, 3139 meshes, 331 cutout materials — **none
of them alpha-tested outright**, every one a blended `NiAlphaProperty` reading the 0.5 stand-in
`Material::getAlphaCutoff` hands a blend — 809 sheets, 1340 BC1 and 428 BC2 textures. BC1's alpha
is one bit and BC2's four, and `AlphaImage` decodes both at every level. `shot` counts the rows:

| view | instances | cutout rows |
|---|---:|---:|
| balmora | 8942 | 3003 |
| vivec | 11104 | 5082 |
| seyda-neen-ship | 8544 | 3656 |

### 1.3 The ceiling, measured

Release harness, `shot --repeat=32`, the `trace` and `air` zones' medians. The second column of each
pair is the same build with the cutout bit left off every row — every mask resolved as though every
microtriangle were known, which is more than a micromap can ever give and is therefore the ceiling:

| view | trace | trace, no cutouts | air | air, no cutouts |
|---|---:|---:|---:|---:|
| balmora | 1.67 | 1.45 | 0.16 | 0.13 |
| vivec | 2.95 | 2.31 | 0.16 | 0.11 |
| seyda-neen-ship | 2.75 | 2.22 | 0.25 | 0.21 |

So the cutout costs the trace 13% at Balmora and 22% at Vivec, and the fog volume's probes a fifth
on the shore. The `upscale` zone is two to three milliseconds beside it and unaffected. What a
micromap recovers of this is what §6 measures.

### 1.4 What is already there to build on

- `AlphaImage` decodes a texture's alpha at every level for `SpriteLightMap`'s bake, keyed on the
  file and made when the texture is opened for upload in `SceneUploader`. That is the host oracle
  the device bake is tested against, and the precedent for a bake keyed on content.
- `TonePass` is a compute pass that samples the scene's bindless textures: it takes the array's
  layout at construction and binds the set as set one. The bake kernel is that shape.
- `StructureStorage` pools opaque driver objects at 256-byte offsets in buffers the tree owns, with
  rooms given back through the graveyard. A micromap is such an object.
- `SceneAcceleration::buildMeshes` describes every triangle geometry in `describeTriangles`, which
  is where the micromap attaches through `pNext`, and `prepareRefit` describes the same geometry
  again for a refit.
- `ShapeFold` has already folded every sheet's back into its front, so a leaf card is one triangle
  and one micromap and not two.
- `MeshRange` knows nothing about materials, and a micromap is a mesh's, baked against the material
  the mesh is worn with. §3.4 gives the mesh that one fact.

## 2. What the field does

- **The specification.** A micromap holds one or two bits per microtriangle, packed LSB to MSB per
  byte, `4^N` microtriangles at subdivision level `N` up to 12, in the spec's own space-filling
  ("bird") order. `VkMicromapTriangleEXT` is `{ uint32 dataOffset; uint16 subdivisionLevel; uint16
  format }`, the offset in bytes. Four states in the two-bit format: transparent, opaque, unknown
  transparent, unknown opaque. At a hit: transparent is *ignored without an any-hit*, opaque commits,
  unknown runs the any-hit as non-opaque. The `ForceOpacityMicromap2StateEXT` ray flag and its
  instance twin collapse unknown to its half, and "never evaluate to non-opaque". The instance and
  ray force-opaque flags are applied before the lookup and the micromap's answer is what stands
  where a micromap is present and `VK_GEOMETRY_INSTANCE_DISABLE_OPACITY_MICROMAPS_EXT` is not set.
  A triangle may instead carry a special index — fully transparent, opaque, unknown transparent,
  unknown opaque — and hold no data. The usage counts a build and an attachment are given must
  equal the triangle counts per format and level. A pipeline traced against such structures
  carries `VK_PIPELINE_CREATE_RAY_TRACING_OPACITY_MICROMAP_BIT_EXT`. A micromap build is ordered
  by `VK_PIPELINE_STAGE_2_MICROMAP_BUILD_BIT_EXT` and `VK_ACCESS_2_MICROMAP_{READ,WRITE}_BIT_EXT`.
- **NVIDIA's OMM SDK** (`NVIDIA-RTX/OMM`, `docs/integration_guide.md`): pick the level so that one
  microtriangle covers about `dynamicSubdivisionScale²` texels; four-state for "pixel-identical
  results" against a single-texture, constant-cutoff alpha test, which is this tree's; two-state
  "for secondary rays, lower LOD objects, or when jaggies are acceptable", with unknowns promoted
  `ForceOpaque` as the recommended promotion because "it generally improves RT performance". On
  mips: "Do not use dynamic mip level at all if possible. Pick a texture mip and always use that
  version"; a per-ray mip wants "conservative OMMs that cover a set of possible texture slices at the
  cost of reduced coverage". The GPU baker is "significantly(!) more efficient" than the CPU one and
  is what runtime baking uses; the CPU one is for cooking offline. Memory is the hazard it names:
  `F_k × 4^N_max × T` bits worst case, so cap the level and measure.
- **Indiana Jones** (NVIDIA technical blog): four-state, `unknownStatePromotion_ForceOpaque`,
  `maxSubdivisionLevel 6`, `dynamicSubdivisionScale 2.0`, `PREFER_FAST_TRACE`. Shadow rays traced
  four-state, "fully conservative"; indirect rays with `gl_RayFlagsForceOpacityMicromap2StateEXT`.
  The trace pass went from 7.90 ms to 3.58 ms on foliage, any-hit samples from 17% to 3%; 95% of
  their micromaps bake in under a second and take under 200 kB, all within a 128 MB budget, and one
  pathological asset took 43 MB until capped.
- **dxvk-remix** (`documentation/OpacityMicromap.md`): the one shipping runtime baker over content
  it does not own — the same situation as this tree. Bakes on the device at BLAS construction,
  throttled, up to level 8, released when disabled.
- **The reference implementation** (`rtxmw/docs/design.md` §1) lists "no opacity micromaps" as the
  largest RT cost multiplier left on its table and never built them. Nothing to lift.

## 3. What to build

### 3.1 The device, the functions, the pipelines

`requirements.cpp` gains `VK_EXT_OPACITY_MICROMAP_EXTENSION_NAME` and the feature
`VkPhysicalDeviceOpacityMicromapFeaturesEXT::micromap`, required as everything here is: a device
without it is a hard failure naming it. `DeviceFunctions` gains `vkCreateMicromapEXT`,
`vkDestroyMicromapEXT`, `vkCmdBuildMicromapsEXT` and `vkGetMicromapBuildSizesEXT`.
`PhysicalDevice::getProperties` gains the two subdivision limits. `TracePipeline` sets
`VK_PIPELINE_CREATE_RAY_TRACING_OPACITY_MICROMAP_BIT_EXT`. `Graveyard` learns to bury a
`VkMicromapEXT`.

### 3.2 What a microtriangle promises — `components/rtx/shaders/micromap.h`

One header both sides read, as `skinning.h` is: the four state codes, the bird curve both ways —
`microtriangleIndex(level, u, v)` and its inverse — as `RTX_SHADER` functions, and the kernel's
push constants. The curve is the spec's own listing, and `omm-lib/src/bird.h` in the SDK is the
second statement of it that the test checks against.

**The state of a microtriangle is decided at level zero, conservatively.** Its three corners land on
the texture through the material's `mTextureTransform`, exactly as `texturePoint` lands a hit; the
texels whose bilinear support touches that footprint are read; the microtriangle is *opaque* where
the least of them is at or above the cutoff, *transparent* where the greatest is below it, and
*unknown* otherwise — unknown opaque where the mean is at or above the cutoff, so a two-state ray
resolves it the way the SDK recommends. Bilinear filtering never leaves the range of the texels it
blends, so this is exact against `sampleDiffuse` at level zero: **every shadow ray answers as it
does today**, and the any-hit runs only where it would have had to decide anyway.

**And the cone rays read that level-zero answer where the micromap is known.** This is the decision
`candidateStops` argued against, so it is taken out loud. The cone averages the mask over a mip so
that one distant pixel tests the mean of the texels it covers; a micromap answers for the
microtriangle the ray landed on. What that changes at distance is the *estimator*, not the
picture: a canopy patch with four texels in ten opaque is a hole to the cone, since 0.4 is under the
cutoff, and four hits in ten to a micromap. The accumulator and the upscaler already integrate a
jittered sample a pixel into a coverage, so what they converge to is the four in ten — which is what
the rasterizer's *blend* of the same mip shows, and this content is blended, not tested (§1.2). The
mean-then-test path is the one that invents a contour. This is the field's practice as well: every
shipping micromap bakes one level and lets the reconstruction integrate. What it costs is
measured in §6, and §7 names the fallback if the pictures say otherwise.

### 3.3 The bake, on the device — `components/rtxvulkan/shaders/micromap.comp`

One lane per triangle, `SKIN_WORKGROUP`-style groups. A lane walks its `4^N` microtriangles in
index order — `index → (u, v)` through the inverse curve — decides each state as §3.2 says, packs
sixteen states to a word and stores words. No atomics and no clear: a lane owns its triangle's run
of the data. Per microtriangle the reads are the texels of a bounding box a texel wider than its
footprint, fetched with `texelFetch` at level zero through the bindless array's own sampler — the
same decode of BC1's punch-through bit and BC2's nibble the trace reads. A footprint that wraps is
read through modulo, as the sampler wraps.

Push constants: the mesh's index and texture-coordinate run addresses, the material's texture slot,
transform and cutoff, the first triangle, the count, the level, and the data address. The kernel
runs in the arrival's own batch after the texture writes and before the micromap build.

**The level is chosen on the host per triangle**, because the build's usage counts must be exact
and the host writes them: `N = clamp(round(log4(texels / 4)), 1, 6)` from the triangle's texture
area — one microtriangle about two texels across, the SDK's and Indiana Jones' number — with the
cap the SDK's memory arithmetic makes necessary. A triangle's data is `4^N / 4` bytes in four-state,
byte-aligned from level one up, laid end to end in the mesh's data buffer. No special indices and
no deduplication in the first cut: a uniform triangle costs the hardware one lookup either way, and
the special index only saves its bytes. §8 keeps them.

### 3.4 The scene says which mesh wears what — settled against the loader

**Where a NIF's material lives.** `NifOsg::Loader::applyDrawableProperties` is handed the shape's
*node* and writes the alpha, the texturing and the `Surface::Material` description into that
node's state set (`nifloader.cpp:2825`, `:3050`); the shape's drawable carries none of its own.
Every controller that moves a mask — `UVController`, `AlphaController`,
`MaterialColorController`, `FlipController` — goes into one `CompositeStateSetUpdater` per node,
attached to that same node as an update or a cull callback (`nifloader.cpp:924`). One NIF shape
is one node, so one drawable stands under one state set.

**What an instance shares.** `SceneUtil::CopyOp` is `DEEP_COPY_NODES | DEEP_COPY_CALLBACKS |
DEEP_COPY_USERDATA` (`clone.cpp:20`): nodes and controllers are copied per instance, and state
sets and plain `osg::Geometry` drawables are shared. So a hundred crates are a hundred nodes over
one drawable and one state set — the material `resolveMaterial` keys on is the same object under
every placement, and **a static mesh wears one material by construction**. `SHARE_DUPLICATE_STATE`
in the optimizer only merges more shapes onto fewer state sets, which is the same direction.

**What an animated instance does.** A node with a controller gets its own state set copy from
`SceneExtractor::animate`, pushed above the node's own, so `shading.back()` at the drawable is
that copy and `Shading::mAnimated` is true there. A hundred instances of a scrolling banner are
therefore a hundred materials over one mesh — and none of them is bakeable, which is the one case
where a mesh wears more than one material and exactly the case the bake refuses. An actor's fade is
a `TransparencyUpdater` on the actor's root, above the body parts' own state sets, so it reaches
the placement as `mFade` and never marks a part's material animated.

**So the two facts are the extractor's to state, in `components/rtx/` and nowhere else:**

- `Material::mAnimated`, set in `readMaterial` from `shading.back().mAnimated`. Constant for a
  material's whole life, because it is a fact about the state set the material is keyed on.
- `MeshRange::mMaterial`, handed to `addMesh` by the extractor, which resolves the material before
  the mesh in `addDrawable` — the same reorder for terrain, water and surfaces. The scene keeps the
  caller's finding, as it keeps `mShape` and `mDeform`. A placement of that mesh with another
  material index, where the mesh's own material is not animated, counts one in
  `ExtractionStats::mWornOtherwise`: the loader says it cannot happen, and the number is what
  says so every frame.

A material rewritten later in a way that changes the bake — its diffuse, its cutoff, its transform,
its translucency — reaches the backend through `getWrittenMaterials`; the backend drops the mesh's
micromap and rebuilds its structure bare, counting that too. Rare, and never silent.

**A mesh gets a micromap when**: its material `isCutout`, is not translucent, is not animated, and
its texture is open. `MeshInstance::mOpacity` under one is the fade of an actor and says nothing
about holes, so a fading actor keeps its micromap and its translucent row, as it keeps its mask.

### 3.5 `SceneMicromaps` — `components/rtxvulkan/scenemicromaps.{hpp,cpp}`

Owned by `ViewScene` beside the acceleration, built and extended with it.

| member | what |
|---|---|
| `mStorage` | a `StructureStorage` of `VK_BUFFER_USAGE_MICROMAP_STORAGE_BIT_EXT` blocks |
| `mMicromaps`, `mRooms`, `mUsage` | per mesh slot: the handle, its room, its one `VkMicromapUsageEXT` per level in use |
| `mLevels` | per triangle of the meshes it holds, the level the host chose |
| `mBake` | the compute pipeline of §3.3, over the texture array's layout |

`plan(scene, textures, meshes)`: for each arrived mesh that §3.4 admits, the levels and the usage
counts and the data size, from the scene's texture coordinates and the array's extents.

`bake(batch, scene, textures, meshes)`: for each such mesh, a data buffer and a
`VkMicromapTriangleEXT` array as batch-transient build inputs — `MICROMAP_BUILD_INPUT_READ_ONLY`
and the address bit, the triangle array host-written, the data written by one dispatch — then
`vkGetMicromapBuildSizesEXT`, a room, `vkCreateMicromapEXT`, and `vkCmdBuildMicromapsEXT` for all
of them at once with one scratch, bracketed by a barrier from the kernel's writes to the micromap
build and one from the micromap build to the acceleration structure build.

`describe(mesh)`: the `VkAccelerationStructureTrianglesOpacityMicromapEXT` for `describeTriangles`
to chain — `indexType = VK_INDEX_TYPE_NONE_KHR`, since every triangle owns the entry at its own
index, `baseTriangle = 0`, the mesh's usage counts, its handle — or nothing for a mesh without one.
`prepareRefit` chains the same description, because an update must describe what the build did; a
rig's texture coordinates never move, so the micromap it was built with stays right through every
pose.

`release(meshes, graveyard)`: the handle and the room, buried like a structure.

The row flags are left as they are. The micromap's answer replaces the opaque flag for the
triangles that have one, and the cutout bit still stands for the meshes §3.4 refused.

### 3.6 The rays

Nothing changes in the shaders for the first measurement: known microtriangles are resolved before
any shader runs, and `candidateStops` is reached only for unknown ones and for the meshes without a
micromap. `traversal.glsl` gains `GL_EXT_opacity_micromap` and, as a second step measured on its
own, `gl_RayFlagsForceOpacityMicromap2StateEXT` on the rays whose aliasing nobody can see: the fog
volume's probes and the ambient rays. The sun's and the lamps' shadow rays stay four-state, as
Indiana Jones keeps them, until a picture says the two-state edge is invisible there too.

### 3.7 Stats

`SceneStats::mMicromappedInstances` and `mMicromapBytes`, printed beside the cutout count by
`shot`, `bench` and `scene`; `ExtractionStats::mUnbakeable` for the animated cutouts and the two
canaries of §3.4.

## 4. In what order

Each step leaves the tree building and every test passing.

1. **§3.1**, with a probe test: one triangle, one two-state micromap of one microtriangle, built and
   attached, and a ray through its transparent half reaching the wall behind. The device's answer
   before anything is written against it.
2. **§3.2**: the header, and the core test of the curve.
3. **§3.4**: the mesh's `mMaterial` and the material's `mAnimated`, with the canary.
4. **§3.5 and §3.3**: the pass and the storage, with the device test of §5 against the host oracle,
   attached to nothing yet.
5. **The attachment**, `verify --views=all`, and the numbers of §6.
6. **§3.6**'s second step, measured on its own.

## 5. What proves it

- **`RtxMicromapCurveTest`**: `microtriangleIndex` and its inverse are a bijection at levels 0 to 4,
  the four children of a microtriangle at level `N` are indices `4i` to `4i + 3` at level `N + 1`,
  and the level-one and level-two orders equal the spec's figure and `bird.h`'s table.
- **`RtxMicromapBakeTest`**, on the device: a quad over a 4×4 `Rgba8Unorm` texture whose top half
  is opaque, bottom half transparent, and one texel one level under the cutoff, baked at level two
  and read back. The sixteen states are hand-computed: opaque above, transparent below, unknown for
  every microtriangle whose footprint touches the seam or the odd texel, and unknown *opaque* or
  *transparent* by the footprint's mean. And the same quad through the host oracle over
  `AlphaImage` — the same conservative rule written on the host — bit for bit.
- **`RtxVisibilityTest`**: a card with a two-by-two checker alpha in front of a wall, four pixels
  per microtriangle. The depth at a transparent microtriangle is the wall's, at an opaque one the
  card's, exactly as it is today without the micromap; the same scene with the material marked
  animated gives the same depths through the any-hit. And a fading placement of the same card:
  translucent row, micromap kept, the wall dimmed through the holes and not through the leaves.
- **`RtxSceneTableTest`**: a cutout mesh arriving takes a micromap and a room, a mesh leaving gives
  both back through the graveyard, and a mesh worn by an animated material takes none.
- **`RtxFrameCostTest`**: unchanged, and it must stay so — nothing here is per frame.
- **`verify --views=all`**: views without a cutout byte-identical; views with foliage differ where
  §3.2 says, and each difference is looked at, not only counted.
- One `shot` with people under synchronization validation and one under GPU-assisted validation.

## 6. What to measure

The table of §1.3 again — `trace` and `air` at Balmora, Vivec and the shore — against the ceiling
in it, in three rounds interleaved with the build before, and the `blas` zone on a crossing for
what the bake and the micromap builds add to an arrival. `mMicromapBytes` at each place against the
structures' own size. Then the second step of §3.6 alone. Report what comes out; the ceiling is a
fifth of the trace and the win is some part of it.

## 7. What could go wrong

- **The distant look.** §3.2 argues the level-zero answer is the truer one under accumulation; the
  pictures decide. The fallback keeps every ray but the shadow rays exactly as today: every cutout
  placement gets a second top-level row with `VK_GEOMETRY_INSTANCE_DISABLE_OPACITY_MICROMAPS_EXT`,
  the two rows told apart by a mask bit — the cone rays trace the bare row and `lightThrough` the
  micromapped one — for a few thousand more rows in the top level and no other change. It is a
  smaller step than the attachment and it needs no second structure.
- **Memory.** The cap at level six and the two-texel scale are the SDK's; the number is
  `mMicromapBytes`, and if a place's micromaps approach its structures' size the level cap comes
  down before special indices go in.
- **Arrival time.** The bake is a dispatch per mesh in the arrival's batch, and an arrival already
  stalls the frame it lands in. If the `blas` zone says the bake shows, dxvk-remix's answer is a
  throttle: the structure built bare and rebuilt with its micromap a frame or two later — the shape
  `CompositeQueue` already gives a chunk's ground.
- **Exact usage counts.** The host chooses every level and no triangle takes a special index, so
  the counts are arithmetic on the host. A device-decided special index would break that, which
  is one reason §3.3 leaves them out.
- **A refit over a micromap.** The update must describe the build's geometry, micromap included;
  `prepareRefit` chains the same description and a test refits a rigged cutout.
- **The alpha the kernel reads.** `texelFetch` through the array's sampler decodes what the trace
  decodes; the test against `AlphaImage` is what says the two decoders agree, format by format.
- **A cutout whose texture is not open** when the mesh is built — the sprite path had the same
  race and answers it by registering the texture at first sight. A mesh whose texture slot holds
  nothing yet takes no micromap and is counted.
- **The Metal backend** has no micromaps. The core carries nothing API-specific — the header of
  §3.2, `MeshRange::mMaterial`, `Material::mAnimated` — and the Metal backend reads none of it.

## 8. Out of scope

- Special indices, deduplication of identical micromaps and near-duplicate merging: the SDK's
  memory savers, taken when `mMicromapBytes` says so.
- Micromaps for the sprites, which are discs the primary ray composites and never triangles.
- Displacement micromaps.
- The two-row split of §7, unless the pictures ask for it.
- The NVIDIA SDK itself as a dependency: its CPU baker matches the kernel's rule for a single
  level, and the tree's own kernel and oracle are a few hundred lines that read the textures the
  device already holds.

## References

- Khronos, `VK_EXT_opacity_micromap`: the proposal
  (https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_opacity_micromap.html), the
  ray traversal chapter's opacity micromap section
  (https://docs.vulkan.org/spec/latest/chapters/raytraversal.html), and
  `VkAccelerationStructureTrianglesOpacityMicromapEXT`
  (https://docs.vulkan.org/refpages/latest/refpages/source/VkAccelerationStructureTrianglesOpacityMicromapEXT.html)
- NVIDIA, Opacity Micro-Map SDK, `docs/integration_guide.md` and `omm.h`
  (https://github.com/NVIDIA-RTX/OMM)
- NVIDIA, *Path Tracing Optimizations in Indiana Jones: Opacity MicroMaps and Compaction of
  Dynamic BLASs*
  (https://developer.nvidia.com/blog/path-tracing-optimizations-in-indiana-jones-opacity-micromaps-and-compaction-of-dynamic-blass/)
- NVIDIA, dxvk-remix, `documentation/OpacityMicromap.md`
  (https://github.com/NVIDIAGameWorks/dxvk-remix/blob/main/documentation/OpacityMicromap.md)
- Microsoft, *D3D12 Opacity Micromaps* (https://devblogs.microsoft.com/directx/omm/)
