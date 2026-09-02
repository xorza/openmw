# Shader Execution Reordering: investigation and plan

Shader Execution Reordering (SER) lets a ray generation shader ask the hardware to regroup the
invocations of a launch so that a warp holds threads about to do the same work, on the same data.
Ada has the hardware for it, this machine's driver exposes it, and the trace kernel is the
divergent, occupancy-bound uber-kernel it was built for. What stands in the way is that the trace
is a compute dispatch, and a reorder can only be asked for from a ray generation shader.

This is the plan for moving the trace into one and reordering it, in two stages. Stage 1 keeps every
line of the trace's own logic and its inline ray queries, changes the launch, builds a hit object
out of each primary query and reorders on it once. NVIDIA's own paper names that exact integration
for existing inline-query renderers, and it can be measured in a day or two. Stage 2 is the full
form — hit objects traced by the pipeline and separate closest-hit shaders per surface kind — which
is also the structural answer to the register cliff the shader review measured. Stage 2 is decided
on Stage 1's numbers.

## 1. What was verified on this machine

Device, driver and toolchain, checked on 2 September 2026:

| Fact | Value |
|---|---|
| Device | NVIDIA GeForce RTX 4090 Laptop GPU, driver 610.57.04, Vulkan 1.4.341 |
| `VK_KHR_ray_tracing_pipeline` | present, `rayTracingPipeline` true |
| `VK_EXT_ray_tracing_invocation_reorder` | present, revision 1, `rayTracingInvocationReorder` true |
| `VK_NV_ray_tracing_invocation_reorder` | present, revision 1 |
| Reordering hint mode | `RAY_TRACING_INVOCATION_REORDER_MODE_REORDER_EXT` — the driver reorders, it does not ignore the hint |
| `shaderGroupHandleSize` / `shaderGroupBaseAlignment` / `shaderGroupHandleAlignment` | 32 / 64 / 32 bytes |
| `maxRayRecursionDepth` | 31 (unused: Stage 1 never calls `traceRayEXT`) |
| `maxRayDispatchInvocationCount` | 2³⁰, against 2.1 M pixels at 1080p |
| `VK_EXT_opacity_micromap` | present (the review's finding 2, and a Stage 2 companion) |
| glslc | shaderc 2026.3 from Vulkan SDK 1.4.357 |

Two probe `.rgen` shaders compile with `glslc -O --target-env=vulkan1.4` and pass `spirv-val`:

- A ray query, `reorderThreadEXT(hint, bits)`, a specialization constant and an image store. The
  SPIR-V carries `OpCapability ShaderInvocationReorderEXT`,
  `OpExtension "SPV_EXT_shader_invocation_reorder"` and `OpReorderThreadWithHintEXT`.
- The same query with a hit object recorded out of it —
  `hitObjectRecordFromQueryEXT(hit, query, recordIndex, attributeLocation)` on a hit,
  `hitObjectRecordMissEXT(hit, rayFlags, recordIndex, origin, tMin, direction, tMax)` on a miss,
  a `hitObjectAttributeEXT vec2` for the barycentrics — then `reorderThreadEXT(hit, hint, bits)`,
  `hitObjectIsHitEXT` and `hitObjectGetShaderBindingTableRecordIndexEXT`. The SPIR-V carries
  `OpHitObjectRecordFromQueryEXT`, `OpHitObjectRecordMissEXT` and `OpReorderThreadWithHitObjectEXT`.

The NV spelling compiles too. Ray queries are legal in a ray generation shader, so the traversal
code in `traversal.glsl` and every caller of it move across unchanged.

## 2. What the sources say

The plan below follows these, and each point is cited at the end.

**The sort key, in order of priority.** NVIDIA's paper and the extension proposal define it the
same way: first the shader identifier held by the hit object, then the coherence hint from its most
significant bit down, then the spatial information in the hit object — which is what groups hits on
the same object together and answers *data* divergence as well as execution divergence. A reorder
by hint alone has only the middle of those three.

**Where to reorder.** After the acceleration structure traversal and before shading, once per
invocation. The paper's basic pattern is trace to a hit object, reorder on it, invoke; it says a
plain substitution of that sequence for a `TraceRay` "can already result in significant
performance gains", and that hints, live-state work and the rest come after. Khronos's guidance is
the same in one line: start with shader-only reordering and add hints if profiling shows a benefit.

**Ray queries are a sanctioned way in.** The paper has a section on it: a hit object "may be used
to create a HitObject based on information obtained from TraceRayInline. This provides an easy path
to integrate reordering into existing applications that use RayQuery" — and the application "might
choose to implement its material shading within the raygeneration shader, and benefit from
reordering by improved data coherence and optionally add coherence hints to reduce execution
divergence." That sentence is Stage 1 of this plan. The EXT extension gives it a name,
`hitObjectRecordFromQueryEXT`, and the shader-table index it records is the application's to choose,
which the extension's own custom-indexing pattern relies on.

**When not to reorder.** Trivial hit shading — shadow and ambient-occlusion rays — is not worth the
cost of extracting coherence; nor are rays that are coherent to begin with, which is what primary
rays are *as rays*. What earns the call is "non-trivial hit shading (irrespective of whether the
shading code lives in the raygeneration shader or in closesthit), paired with at least moderate
divergence" in what the rays land on, and "a high shader count isn't necessarily required." In a
loop of rays from one origin with cheap shading, reorder once before the loop; in a path tracer,
reorder for each radiance ray and not for the one-off shadow ray.

**A caveat that applies here.** Reordering by hit location "means giving up on the 2D screen-space
locality that physical threads have by default. This can negatively impact the performance of
reading gbuffer data or writing output buffers." This trace writes eleven channels, 94 bytes a
pixel, at its end. The reorder's win on the shading has to be larger than what those writes lose.

**Hint bits.** Use the fewest that carry a real branch, ask for the same count on every thread, and
do not repeat in the hint what the shader identifier already says: hint bits displace hit-object
information from the key, and an implementation "is free to ignore an arbitrary number of least
significant bits." An estimate is enough — reordering affects performance and not correctness, so
"an approximate coherence hint is often better than none at all." The most effective single bit in
a loop is whether the thread is about to leave it. The GLSL spec bounds `bits` at 32.

**Control flow.** A reorder may be reached under divergent control flow; only the threads that
arrive at it are reordered. Where possible the implementation "will also attempt to retain locality
in the thread's launch indices." The scope is, by measurement, about one streaming multiprocessor's
worth of warps, sorted by radix on the hint bits, with live state spilled to L2.

**Live state is the cost.** Everything live across the call is packed to memory and unpacked after
it, so the call's cost is proportional to it, and the compiler removes much of it — which is why
guessing at it from the source is unreliable. Nsight Graphics's shader profiler reports the spilled
bytes per call site and the variables they came from; Indiana Jones took its call site from 222 to
84 bytes by turning a once-run loop into a branch, halving precision on accumulators, packing a
direction into a word and moving demodulation to a later pass, and SER's saving on that pass went
from 11 to 24 percent. Nsight's "Active Threads Per Warp" counter is what shows coherence per line
before and after.

**Why ray generation and not compute.** Compute has explicit thread grouping and workgroup memory;
ray generation gave those up, which is what makes the launch reorderable. The kernel here uses
neither.

**What the field measured.** A single-übershader Vulkan glTF path tracer: 47.8 percent faster, warp
coherence 23 to 54 percent. Indiana Jones: 24 percent on its path-tracing pass. Alan Wake 2: 39
percent of its ray tracing cost. Cyberpunk 2077: 24 percent off `DispatchRays`. Black Myth: Wukong's
ReSTIR GI: 3.7 times. Microsoft's synthetic sample: 40 percent on a 4090 — with its own warning
that synthetic figures do not carry into games. The übershader case is the one shaped like this
renderer.

## 3. Why this renderer wants it

The trace is one kernel for every pixel, and what a pixel does depends on what it hit:

- **The primary hit's kind.** Sky (14 percent of pixels at the ship at Seyda Neen), water (two more
  traversals, a shaft march and a shore ray), terrain with a layer stack (four or five masked
  texture reads), a plain textured surface, a pane the eye sees through (a second traversal).
- **The bounce.** A ray that escapes takes the sky; one that lands shades a second surface with its
  own lamp reservoir and ambient ray; far ground out of doors takes neither.
- **The lamps.** The reservoir walks whatever the light grid's cell holds, which differs by
  hundreds between a street and a hall.
- **The data.** Neighbouring pixels land on different instances, materials and textures; the hit
  object's spatial key is what groups those, and no hint can.
- **Cutouts in traversal.** 3656 of 8544 instances at Seyda Neen are cutouts, and a candidate on
  one reads a texture inside the traversal loop of every ray that crosses it. A reorder cannot reach
  that; opacity micromaps can.

`lib/variants.glsl` records that the kernel is occupancy-bound, and the shader review measured the
other face of that: the driver compiles the exterior kernels at 96 registers with spills where it
chose 128 a few edits earlier, and the trace is a tenth of a millisecond slower for it. SER sorts
the divergence; Stage 2 also shrinks the live state each shader carries, which is what moves the
kernel off that edge.

Today's numbers to beat, `trace` zone medians from the harness bench at 1280×720 traced:

| View | trace ms | registers |
|---|---|---|
| seyda-neen-ship | 1.67 | 96, spilling |
| seyda-neen-ship-dawn | 2.15 | 96, spilling |
| balmora-mages-guild | 1.57 | 96, spilling |

`VISIBILITY_STRIP`, the hand-written workgroup permutation in `visibility.comp`, is worth 0.2 ms at
the ship by the measurement written beside it. It is a coherence trick a launch order cannot carry,
and SER is the mechanism it stood in for.

## 4. What a reorder needs

- **Ray generation stage only.** `reorderThreadEXT` and the fused calls are defined for that stage
  and no other. Inline ray queries stay.
- **Small live state at the call.** After a traversal has answered and before anything is resolved
  off the answer: the pixel, the ray, the cone and the hit record. Nothing computed before the call
  is read after it except those.
- **One call, reached by every invocation.** The hint is what differs between them; the count of
  hint bits does not.
- **A hit object per primary query**, recorded with a shader-table index the trace chooses, so the
  sort's first key is the kind of shading ahead and its last is where the hit is.
- **A ray tracing pipeline.** One ray generation stage, one shader group, a shader binding table of
  one 32-byte handle in a 64-byte-aligned buffer, `vkCmdTraceRaysKHR` with empty miss, hit and
  callable regions. No `traceRayEXT`, so `maxPipelineRayRecursionDepth` is one and nothing recurses.
- **Stage flags.** Set 0 is the pipeline's own. The three sets bound after it are made once with
  `VK_SHADER_STAGE_COMPUTE_BIT` and are shared with the fog volume, which stays compute: the bindless
  textures (`texture.cpp:70`), the G-buffer channels (`gbuffer.cpp:225`) and the fog volume
  (`fogvolume.cpp:54`) take `VK_SHADER_STAGE_RAYGEN_BIT_KHR` beside it.
- **Barriers.** Everything that hands the trace an input or takes its output names
  `VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT` today, sixty times across fifteen files. The ones that
  are about the trace gain `VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR`; synchronization
  validation is the gate that says which were missed.

## 5. Stage 1: the trace as a ray generation shader with one reorder

### 5.1 Device

`requirements.cpp`: `VK_KHR_ray_tracing_pipeline` and `VK_EXT_ray_tracing_invocation_reorder`
join the required extensions; `VkPhysicalDeviceRayTracingPipelineFeaturesKHR::rayTracingPipeline`
and `VkPhysicalDeviceRayTracingInvocationReorderFeaturesEXT::rayTracingInvocationReorder` join the
feature table. `DeviceProperties` chains `VkPhysicalDeviceRayTracingPipelinePropertiesKHR` for the
handle size and alignments, and `VkPhysicalDeviceRayTracingInvocationReorderPropertiesEXT` so
`openmw-rtxtool info` prints the hint mode. A device whose mode is `NONE` is refused with the mode
named: this tree does not keep a path for a driver that ignores the hint.

`device.cpp` loads `vkCreateRayTracingPipelinesKHR`, `vkGetRayTracingShaderGroupHandlesKHR` and
`vkCmdTraceRaysKHR` the way it loads the acceleration structure entry points.

The EXT spelling throughout. Both are in the driver and the headers; the EXT one is the promoted
form, the one the layers are current for, and the one whose hit-object-from-query call the probe
compiled.

### 5.2 Pipeline

A `TracePipeline` beside `ComputePipeline`, with the same shape — set-0 layout, pipeline layout and
pipeline as one object, the push-descriptor set at zero, specialization words by index, statistics
captured at creation and reported through `Device::reportPipeline`. The layout creation the two
share is lifted into one function both call. What is new is the shader binding table: one
device-local buffer per pipeline, `SHADER_BINDING_TABLE` and `SHADER_DEVICE_ADDRESS` usage, the
group handle written at offset zero, and the three `VkStridedDeviceAddressRegionKHR` the launch
takes kept on the object.

`VisibilityPass::compileEvery` makes sixteen of these in place of sixteen compute pipelines, on the
same threads and the same cache. The fog volume's eight stay compute.

### 5.3 Shader

`visibility.comp` becomes `visibility.rgen`, listed in `RTX_SHADERS` under that name; glslc reads
the stage off the extension. The differences:

- `gl_LaunchIDEXT.xy` is the pixel; `gl_LaunchSizeEXT` is the frame. No workgroup, so no bounds
  test, no `VISIBILITY_WORKGROUP` and no `stripedWorkgroup`. The constant leaves `visibility.h`
  with the dispatch that read it.
- `#extension GL_EXT_ray_tracing` and `GL_EXT_shader_invocation_reorder` join the list, and a
  `layout(location = 0) hitObjectAttributeEXT vec2` carries the barycentrics into the record.
- `trace()` in `traversal.glsl` splits into what it already is inside: `traverse()`, which runs the
  query and returns a `Hit` — hit or not, instance, primitive, barycentrics, distance — and
  `resolve()`, which turns a `Hit` into a `Surface`. Every present caller of `trace()` calls the
  pair. The primary ray in `main` calls `traverse()`, records, reorders, then `resolve()`:

  ```glsl
  const Hit hit = traverse(origin, direction, 0.0, MASK_SOLID | MASK_WATER | MASK_FIRST_PERSON);

  // The kind of shading ahead is the hit object's shader-table index: the sort's first key, and
  // nothing else is executed off it. Where the hit is, the hardware reads for itself.
  hitObjectEXT key;
  if (hit.mHit)
      hitObjectRecordFromQueryEXT(key, ..., reorderKind(hit), 0);
  else
      hitObjectRecordMissEXT(key, gl_RayFlagsNoneEXT, KIND_SKY, origin, 0.0, direction, frame.mFar);

  reorderThreadEXT(key, reorderHint(hit), REORDER_BITS);

  Surface surface = resolve(hit, ...);
  ```

  `reorderKind` reads one instance row and one material row — what `resolve` reads first anyway —
  and answers water, terrain with a stack, or surface. `reorderHint` carries what the kind does
  not: whether the eye sees through this surface, and whether the material is a cutout. Two bits.
  The eye's under-water state is the same for every pixel of a frame and so is not a hint. The pane
  case re-traverses after the reorder, which is the rare path and the right side of the call for it.
- Since the query object is consumed by the record inside `traverse()`, `traverse()` records the
  hit object itself and hands it back in the `Hit`; `hitObjectEXT` is declarable in a ray generation
  shader without storage qualifiers. The exact form is settled when the file is written.
- `REORDER` is a specialization constant with three values: no call at all, the hit object alone,
  the hit object with the two-bit hint. The harness takes it on the command line, so the launch
  change and the two reorder forms are three builds of one shader.
- Everything else is untouched: the sky, the water, the fog, the sprites, the bounce, the channel
  writes, the hit counter.

### 5.4 Recording

`VisibilityPass::record`: bind point `VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR` for the pipeline,
the pushed set and the three bound sets; `vkCmdTraceRaysKHR(width, height, 1)` in place of the
dispatch. `writeConstants`' second barrier and the fog volume's `handOver` name the ray tracing
stage as their destination. The GPU timer zones are unchanged.

### 5.5 Tests and verification

- A device test beside `RtxProbeTest`: a ray generation pipeline that writes `gl_LaunchIDEXT` into
  a buffer, launched over a small grid, read back and compared. It is the shader binding table and
  the launch, asked directly.
- `openmw-rtxtool verify` against a run of the compute trace: every view byte-identical. The
  arithmetic per pixel does not change, so the picture must not; a reorder changes which lanes
  share a warp and nothing a lane computes.
- The 508 `Rtx*` tests, and a `--sync-validation` pass over the default bench suite for the
  barriers. Validation layers on for the first runs of every `REORDER` value, since a recorded
  shader-table index that names no record is an assumption the layers may have an opinion about.
- Register counts read off the pipeline statistics for all sixteen variants, before and after,
  because an RT pipeline is compiled under a different allocation policy and the direction is not
  known until it is read.

### 5.6 Measurement

Alternating snapshots of the release harness, three runs each, default suite plus `exteriors` and
`interiors`, in this order:

1. Compute trace, as today.
2. Ray generation trace, `REORDER` off. This is the launch change alone: the strip permutation
   gone, the driver's own launch order in its place, the RT pipeline's register policy.
3. Hit object alone. The kind and the hit's place, no hint — the "start with shader-only
   reordering" step.
4. Hit object with the two-bit hint.
5. Only if 3 or 4 pays: a second call after the bounce's traversal, keyed on a hit object recorded
   from that query, with one hint bit for whether the bounce shades at all — the paper's loop-exit
   bit. The shadow rays and the ambient ray are never reordered for; the paper says why.

The number that decides is the `trace` zone median; the p99 and the worst frame are read beside it.
`air`, `upscale` and the rest are the control: none of them changes. Beside the timer, two things
are read where the tool allows: Nsight Graphics's "Active Threads Per Warp" on the shading lines
before and after, and its "Ray Tracing Live State" bytes at the call site. Nsight Graphics is not
installed on this machine today; until it is, the pipeline statistics' spill size is the proxy for
the second and the timer stands in for the first.

### 5.7 Effort

One to two days: the device and pipeline work is a morning, the shader split and the launch an
afternoon, and the rest is the barrier sweep under synchronization validation and the runs.

## 6. Stage 2: hit objects traced by the pipeline, closest-hit shaders per kind

Stage 1 sorts the threads and leaves the uber-kernel whole. The full form is a pipeline in which
the traversal itself produces the hit object, the reorder sorts on the shader that object names,
and the shading runs as that shader — so a warp is not merely lanes that will take the same branch,
it is lanes running the same, smaller program. That is also what takes the register cliff away:
each closest-hit shader carries only its own live state, and the trace stops being one kernel that
has to hold the union.

The shape, following the paper's patterns:

- **Ray generation** builds the primary ray, calls `hitObjectTraceRayEXT`, and reorders and invokes
  with the fused `hitObjectReorderExecuteEXT(hit, hint, bits, payload)`, which the extension says
  may be cheaper than the two calls apart. It then takes what the closest-hit shader wrote — the
  `SurfaceResponse`, the direct light, the surface's position and normal — and does the bounce the
  same way, with the loop-exit bit in that hint. The fog, the sprites and the channel writes stay in
  the ray generation shader, where they are already the tail of the frame.
- **Common work between trace and execute.** The paper's "common computations with hit coherence"
  pattern: what every kind shares — the lamp reservoir walk, the ambient ray — can run in the ray
  generation shader after the reorder and before the execute, coherent by the same sort, and leave
  the payload.
- **Closest-hit shaders** per `MaterialKind`: surface, terrain, water; a **miss** shader for the
  sky. Each is `resolve` and `shadeSurface` for its own kind, and each carries its own registers.
  The water's own two traversals stay ray queries inside its closest-hit shader.
- **Any-hit** for cutouts, which is `candidateStops` as a stage — or, with the review's finding 2,
  opacity micromaps in the build so that only the unknown state reaches it. The any-hit's payload
  is the small one: the paper's advice is to tailor the payload to the shader invoked, and an alpha
  test wants none of what the closest-hit returns.
- **The payload** is what the closest-hit shader hands back: the response, the radiance, the
  position, the normal, the footprint and a few flags. Under sixty-four bytes, packed the way
  Indiana Jones packed its — half floats where the precision allows, a direction in a word.
- **The shader binding table** gains hit groups by kind. `SceneAcceleration` writes each instance's
  `instanceShaderBindingTableRecordOffset` from its material's kind, which today is a table read
  and becomes an index the hardware follows — or the raygen sets it on the hit object with
  `hitObjectSetShaderBindingTableRecordIndexEXT`, the custom-indexing pattern, and the instances
  stay as they are.

Open questions Stage 2 has to answer first:

- Whether the shadow rays and the lamp reservoir stay ray queries inside the closest-hit shaders or
  move to the ray generation shader's common section. The paper's rule — no reorder for a one-off
  shadow ray — argues for leaving them where the surface is, and the common-work pattern argues
  for the reservoir walk in the middle.
- What the DLSS guide channels cost to route through a payload rather than being written where they
  are computed.

Effort: one to two weeks, after Stage 1's numbers say it is worth it. If Stage 1's reorder buys
less than a few percent, the divergence is not where the time goes and Stage 2 is worth it only for
the register relief — which is measurable on Stage 1's own register report.

## 7. Risks

- **The RT pipeline's register policy.** NVIDIA compiles ray generation shaders under different
  limits than compute. The direction is unknown until the statistics are read; step 2 of the
  measurement isolates it.
- **The reorder's own cost against the G-buffer writes.** The paper's own caveat: sorting by hit
  scatters the screen, and this trace writes 94 bytes a pixel at its end. Step 3 against step 2
  is that trade measured. If it loses, the answer is not to abandon the reorder but to write less —
  the review's finding 4 — and to measure again.
- **The recorded shader-table index.** Stage 1 records indices that name no hit group and never
  executes them. The extension's custom-indexing pattern rests on the index being data until it is
  executed, and the layers are asked before the bench is.
- **Barriers.** Sixty stage references, of which perhaps twenty are about the trace. Synchronization
  validation finds the ones that are missed; a missed one is a wrong picture and not a crash.
- **Compile time.** Sixteen RT pipelines on a cold cache, in parallel, through the same pipeline
  cache. The trace took 2.8 s cold as compute; an RT pipeline is expected to be similar and the
  cache makes the second run free.
- **What it does not fix.** The cutout candidate loop runs inside traversal, before any reorder can
  help; that is opacity micromaps. And the fog volume stays a compute kernel, which is fine: it was
  never the divergent one.

## 8. Acceptance

Stage 1 lands when all of these hold:

- Every `verify` view byte-identical to the compute trace.
- The 508 tests pass, and the bench suite runs clean under synchronization validation.
- The `trace` zone median is no worse at any of the default suite's three views with `REORDER`
  off, and better at both exteriors with the hit object.
- The register report for every variant is in the log, and the review's `shader-review.md` gets
  the numbers.

## 9. Sources

- NVIDIA, *Shader Execution Reordering* whitepaper, v1.0: the sort key's three components in
  priority order, the trace / reorder / invoke pattern, the hint-bit and live-state guidance, the
  "when not to use" section, and the ray-query integration section.
  <https://d29g4g2dyqv443.cloudfront.net/sites/default/files/akamai/gameworks/ser-whitepaper.pdf>
- Eric Werness, NVIDIA, *Execution Reordering for Ray Tracing Performance*, Vulkanised 2025: the
  EXT function set, `hitObjectRecordFromQueryEXT`, custom shader-table indexing, the fused calls,
  and the hint-bit rules.
  <https://vulkan.org/user/pages/09.events/vulkanised-2025/T49-Eric-Werness-NVIDIA.pdf>
- Khronos, *GL_EXT_shader_invocation_reorder* specification: signatures, stages, the bound on hint
  bits.
  <https://github.com/KhronosGroup/GLSL/blob/main/extensions/ext/GLSL_EXT_shader_invocation_reorder.txt>
- Khronos, *VK_EXT_ray_tracing_invocation_reorder* proposal.
  <https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_ray_tracing_invocation_reorder.html>
- Khronos blog, *Boosting Ray Tracing Performance with Shader Execution Reordering*: the measured
  results across applications and the best-practice list.
  <https://www.khronos.org/blog/boosting-ray-tracing-performance-with-shader-execution-reordering-introducing-vk-ext-ray-tracing-invocation-reorder>
- Vulkan Samples, *ray_tracing_invocation_reorder*: the reference integration and its hint.
  <https://docs.vulkan.org/samples/latest/samples/extensions/ray_tracing_invocation_reorder/README.html>
- NVIDIA developer blog, *Path Tracing Optimization in Indiana Jones: Shader Execution Reordering
  and Live State Reductions*: the live-state method, tools and numbers.
  <https://developer.nvidia.com/blog/path-tracing-optimization-in-indiana-jones-shader-execution-reordering-and-live-state-reductions/>
- Microsoft, HLSL proposal 0027 *Shader Execution Reordering*: control-flow semantics, hint-bit
  significance, `HitObject::FromRayQuery`.
  <https://microsoft.github.io/hlsl-specs/proposals/0027-shader-execution-reordering.html>
- Microsoft DirectX developer blog, *D3D12 Shader Execution Reordering*: the sample's sort key and
  its caveat on synthetic numbers.
  <https://devblogs.microsoft.com/directx/shader-execution-reordering/>
- Chips and Cheese, *Shader Execution Reordering: Nvidia Tackles Divergence*: the reorder's scope,
  the radix sort on hint bits, the L2 spill path and the Cyberpunk figures.
  <https://chipsandcheese.com/p/shader-execution-reordering-nvidia-tackles-divergence>
- Slang, *Shader Execution Reordering* documentation: launch-index locality and the same-bit-count
  advice.
  <https://github.com/shader-slang/slang/blob/master/docs/shader-execution-reordering.md>
