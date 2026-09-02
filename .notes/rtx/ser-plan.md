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

Stage 1 was accepted on these, which are not the ones written here first. §9 is what changed them
and why.

- Every `verify` view byte-identical to the compute trace **with `REORDER` off** — which it is not,
  and cannot be. §9.2.
- The 509 tests pass, and the bench suite runs clean under synchronization validation.
- The `trace` zone median is better at all three of the default suite's views with `--reorder=off`,
  and worse at all three with every form of the reorder. §9.1.
- The register report is not in the log, because this driver will not give one for a ray tracing
  pipeline. §9.4.
- Every form the API has was measured, not only the two §5.6 listed. §9.5 is the whole audit
  against the sources.

## 9. What Stage 1 measured

Written after the fact, against what §5 and §7 expected. The code is landed; the reorder is off by
default, which is what these numbers say it should be.

### 9.1 The launch is worth having and no form of the reorder is

`trace` zone medians of three alternating runs apiece, release build, no layers, the default suite
at 1920×1080 presented and 1280×720 traced — the same measurement §3's table was taken with. The
compute column was measured the same way from the tree before this change, and reproduces §3 to a
hundredth.

The four reorder columns are the three forms the API has, all at the eye, and then the plainest of
them — the hit object with no hint, which is where the sources say to start — moved off the eye's ray
onto the bounce. `--reorder` names one.

| View | compute | launch, `off` | `hit` | `hint` | `both` | `bounce` |
|---|---|---|---|---|---|---|
| seyda-neen-ship | 1.69 | **1.56** | 1.77 | 1.70 | 1.78 | 1.88 |
| seyda-neen-ship-dawn | 2.18 | **1.96** | 2.28 | 2.17 | 2.30 | 2.36 |
| balmora-mages-guild | 1.59 | **1.49** | 1.60 | 1.63 | 1.60 | 1.94 |
| against the launch | — | — | +7 to +16% | +9 to +11% | +7 to +17% | +20 to +30% |

**The launch alone is 6 to 10 percent faster than the dispatch it replaces**, and it gives up
`VISIBILITY_STRIP` to get there — the hand-written workgroup permutation measured at 0.2 ms over
Seyda Neen. So the driver's own launch order beats a row-major dispatch by more than that
permutation recovered, and that is the whole of Stage 1's win. NVIDIA's Vulkanised deck says why in
one line: the ray pipeline already carries "a limited definition of ray generation launch grouping"
and an "invocation repack instruction", so the launch was reordering something before this fork
asked it to.

**No form of the call pays, and what each costs says why.** The three differ in exactly one thing —
how much of the launch's own 2D neighbourhood the sort gives up — and they line up with it:

- `hint` sorts on four hint bits and carries no hit object at all, so a thread keeps its place in the
  launch. Outdoors it is the cheapest of the three by four to six points.
- `hit` and `both` sort on the hit object, whose key ends in where the hit is. That is the
  whitepaper's own caveat: reordering by hit location "means giving up on the 2D screen-space
  locality that physical threads have by default", which "can negatively impact the performance of
  reading gbuffer data or writing output buffers". This trace ends in eleven of those, 94 bytes a
  pixel.
- Indoors the order reverses — `hint` is the *worst* of the three. A room holds one kind of surface
  and few materials, so four bits of hint sort nothing and the hit object at least groups by
  instance.

**The bounce is worse than the eye, which is the one result the sources did not predict.** §2 quotes
them saying primary rays are coherent to begin with and that a secondary scattered ray with
non-trivial shading is where reordering shines. One diffuse bounce from the eye is exactly that ray,
and reordering it costs 20 percent outdoors and 30 in a room. The room says why: a bounce there is
short and lands on the same few surfaces, so there is no coherence to recover — and what has to
cross the call is the primary `Surface` the frame still owes eleven channels to.

**So this renderer is not divergence-bound where SER can reach.** The divergence it has at the
primary hit is small, the divergence it has inside traversal is the cutout loop that no reorder
touches, and the work after any reorder here is a G-buffer write that wants its threads left where
they were.

Two smaller changes went in beside the measurement and are in the `off` column: the `Hit` record
dropped the object-to-world matrix for the one world-space vector it was used to make — nineteen
words to thirteen — and a trace that reorders nothing is built with no miss or hit records at all.
Together they are worth about 0.02 ms at every view, which is the edge of the run-to-run spread.

### 9.2 The reorder changes the picture, and the plan said it could not

§5.5 asked for byte identity on the grounds that "a reorder changes which lanes share a warp and
nothing a lane computes". The first half is true and the second is not, for a reason that is the
compiler's rather than the hardware's.

**The call is value-neutral by itself.** Moved to the end of `main`, after every channel has been
written, the frame comes back byte for byte the one the launch drew without it. Left where it is
useful it is a barrier the driver rebuilds the code around — live state is rematerialised rather
than spilled, a multiply–add contracts on one side of it and not the other — and this renderer takes
one sample per pixel of a bounce and one reservoir draw over the lamps, so a last-bit difference
turns into a different lamp or a different bounce direction on a small number of pixels.

Measured over the twenty `verify` views at 1920×1080, against `off`: the three forms at the eye each
move **every view, 0.00 to 0.04 percent of pixels, worst 97 of 255 on those**, and move the same
pixels as each other. `bounce` moves **seven views of twenty, worst 24 of 255** — it leaves the eye's
own shading and all eleven writes alone, so what it can move is the one bounce sample and only where
a bounce is traced at all. The differing pixels are scattered rather than clustered — a third have a
differing neighbour, which is what the wavelet spreading a firefly looks like — and most differ by
one to eight of 255.

**The launch alone is not byte-identical to the compute trace either**, for the same reason with one
more cause: a ray generation shader is compiled under a different register allocation policy than a
compute one, so the arithmetic is scheduled differently before any reorder is asked for. `off`
against the compute trace: 0.01 to 0.43 percent of pixels, worst 161 of 255.

So byte identity is not the oracle for this change, and `verify --against` is read as a scatter
count rather than a pass or a fail. What is byte-identical is `sorted` against `hinted`, and a run
against itself.

### 9.3 An empty shader table is a device loss, not a validation message

§7 expected the layers to have an opinion about a recorded index that names no hit group. They have
none: `vkCmdTraceRaysKHR` with empty miss and hit regions, a hit object recorded into them and a
reorder on it, loses the device — `VK_ERROR_DEVICE_LOST` at the next wait, with the layers and
synchronization validation both silent.

**The sources say so plainly, and this plan read them the other way round.** NVIDIA's reference for
`NvMakeMiss` is "the provided shader table index must reference a valid miss record in the shader
table", and `NvMakeHitWithRecordIndex` says the same of a hit group. Microsoft's proposal says why:
"MaybeReorderThread may access both information about the instance in the acceleration structure as
well as the shader record at the shader table offset contained in the HitObject." The index is not
data until it is executed — the reorder itself reads it.

So the table has the records it names: one miss shader for the sky and one closest-hit shader per
`MaterialKind`, none of them ever invoked. **Their own files rather than one shader named three
times**, because the sort's first key is the shader the record names and three records of one shader
may carry one identifier. `RtxTracePipelineTest` is that failure asked of the device directly, and it
also asks the one thing a reorder must leave alone: an invocation's launch index, read from either
side of the call.

### 9.4 The register count is gone

NVIDIA's compiler reports one executable for every compute pipeline in this renderer and **none at
all** for a ray tracing one, so `VK_KHR_pipeline_executable_properties` gives no register count and
no spill size for the trace. `Device::reportPipeline` says so per pipeline rather than logging
nothing, and the flag stays asked for.

That takes away the number §3 stated the problem in and the number §6 would decide Stage 2 on. What
is left is the timer, and Nsight Graphics — which is not installed on this machine.

### 9.5 What the sources ask for, and what this does

Read against NVIDIA's whitepaper, its Vulkanised 2025 deck, the GLSL and Vulkan specifications,
Khronos's best-practice post and the Indiana Jones live-state article. Each of these is §11.

- **Reorder after traversal and before shading, once per invocation.** Done, and the miss is
  recorded too so that every invocation reaches one call.
- **The same hint-bit count on every thread.** A specialization constant, so the count is fixed for
  the whole launch.
- **Use the fewest hint bits that carry a real branch.** Two with a hit object; four without one,
  where the kind has to go in the hint because there is no shader identifier to carry it.
- **More significant hint bits weigh more.** The kind sits above the flags, and of the two flags the
  one that decides a second traversal sits above the one that decides a texture read.
- **Do not repeat in the hint what the shader identifier says.** The hit-object forms hint on the two
  flags alone; the kind is the record's.
- **All three forms of the call.** `reorderThreadEXT(hitObject)`, `reorderThreadEXT(hint, bits)` and
  the two together. The middle one is the one this renderer's G-buffer writes want, and it was the
  one Stage 1 was first written without.
- **Live state is the cost.** The record that crosses the call is thirteen words: what the query
  answered, plus the triangle's plane and the vertex normal, both already in world space. The
  object-to-world matrix that made them does not cross.
- **A valid record behind every index.** §9.3.
- **Where the divergence is.** Tried at the eye and at the bounce. §9.1.

Not done, and why:

- **Nsight Graphics's live-state and warp-coherence counters**, which the sources say are how this is
  tuned properly. Not installed on this machine, and §9.4 is what took the fallback away.
- **The fused calls and closest-hit shaders per kind.** Stage 2, and Stage 1's numbers say to spend
  that effort elsewhere.
- **Opacity micromaps.** The one thing the sources point at that does reach this renderer's
  divergence: 3656 of 8544 instances at Seyda Neen are cutouts, and a candidate on one reads a
  texture inside the traversal loop of every ray that crosses it. No reorder can reach inside
  traversal. §6 already lists it, and it is now the next thing to do rather than a companion.

### 9.6 One bug the launch found

`fogVolumeAlong` sampled the fog volume with an implicit level of detail. In a compute shader the
derivative comes from the quad the workgroup put the thread in; a ray generation shader has no such
neighbour, and after a reorder it is another pixel of the frame entirely. The volume has one level,
so `textureLod(..., 0.0)` is what it always meant, and the compute trace's own frame is unchanged by
the fix.

## 10. What Stage 2 measured

Written after the fact, against what §6 expected, and then a second time after the sources were read
back against the code. §10.5 is what that reading changed.

### 10.1 The split costs about three percent and buys nothing back

`trace` zone medians, release build, no layers, the default suite at 1920×1080 presented and
1280×720 traced — the same measurement §3 and §9.1 were taken with. Five alternating runs of the
Stage 2 column, two of them thrown away with the whole frame slower and the clock down, which is the
board on its power cap rather than a result.

| View | compute (§3) | launch, one kernel (§9.1) | shader per kind | against the launch |
|---|---|---|---|---|
| seyda-neen-ship | 1.69 | **1.56** | 1.62 | +3.8% |
| seyda-neen-ship-dawn | 2.18 | **1.96** | 2.02 | +3.1% |
| balmora-mages-guild | 1.59 | **1.49** | 1.54 | +3.4% |

**The register relief §6 was for did not appear, and §9.4 is why nobody can say by how much.** The
driver reports no executable for a ray tracing pipeline, so the register count that stated the
problem in §3 and would have settled this cannot be read for either shape. What is left is the
timer, and the timer says the divided frame is slower than the kernel that held the union.

**What it plausibly goes on is the payload.** Stage 1 kept thirteen words live across its reorder —
what the query answered and two world-space vectors. Stage 2 keeps twenty-six across the execute,
because the shader that ran has to hand back everything the frame's tail reads and cannot recompute:
the radiance, the bounce, the four terms of the upscaler's response, the mirror a water pixel
reflects, and the opacity the launch peels a pane on. That is a hundred and four bytes a lane
through a call, against fifty-two through a sort.

### 10.2 The reorder still does not pay, and now it had something to sort into

This is the one measurement Stage 1 could not make. Its sort regrouped threads that all went on to
run the same code, so §9.1 could say only that sorting cost more than it saved. Here the sort is
followed by four different programs picked by traversal, which is the arrangement the sources
describe — and the answer is the same.

| View | off | `hit` | `hint` | `both` |
|---|---|---|---|---|
| seyda-neen-ship | **1.62** | 1.92 | 1.92 | 1.92 |
| seyda-neen-ship-dawn | **2.02** | 2.53 | 2.41 | 2.43 |
| balmora-mages-guild | **1.54** | 1.75 | 1.74 | 1.71 |

**Twelve to twenty-five percent, in the shape §9.1 already found**, and the reason has not changed:
the trace ends in eleven channel writes, 94 bytes a pixel, laid out along the launch's own
neighbourhood — and every form of this call gives some of that neighbourhood up. The fused calls
§10.5 brought in moved none of it, so what costs is the sort and the scatter rather than the number
of instructions the API takes to ask for one.

**The whitepaper's own "when not to use" section describes this frame exactly**: primary rays, which
are coherent to begin with, and work after the reorder that reads and writes screen-space buffers.
"Reordering threads with respect to their ray hit location means giving up on the 2D screen-space
locality that physical threads have by default. This can negatively impact the performance of
reading gbuffer data or writing output buffers, which can turn reordering into a net loss when the
rest of the workload is particularly cheap." Two stages of this plan measured that sentence.

### 10.3 What §6 asked for, and what turned out to be true

- **The fused calls exist.** `hitObjectReorderExecuteEXT` and `hitObjectTraceReorderExecuteEXT` are
  both in `GL_EXT_shader_invocation_reorder`, with and without a hint. §6 named the first and this
  plan briefly recorded that neither did — an unverified claim from a probe that never tried them.
  §10.5 is what using them is worth.
- **An any-hit shader is not optional.** §6 listed it beside opacity micromaps as a choice. It is
  neither: an inline query drives its own candidates and `hitObjectTraceRayEXT` does not, so without
  `visibility.rahit` traversal commits the first triangle it meets and every leaf goes back inside
  the card it was painted on.
- **`Reorder::Bounce` is gone.** The bounce is traced inside a closest-hit shader now, and the
  reorder is a ray generation instruction. §9.1 measured that mode worst of the five, so nothing was
  lost.

### 10.4 What Stage 2 is worth keeping for

Nothing the timer can see. What it is: the sky, the water, the layer stack and the plain surface are
four programs rather than four branches of one, each carrying its own live state — which is the
shape every source describes and the shape opacity micromaps and any further per-kind work would
build on. It costs three percent to hold that shape today.

**The decision it forces is whether three percent is worth a shape.** On this renderer's own posture
— picture first, then performance — it is not yet, because the shape buys no picture. §9.5's next
thing to do is unchanged: opacity micromaps, which reach the divergence inside traversal that no
reorder and no split can.

### 10.5 What reading the sources back against the code was worth

Three of the whitepaper's best practices had been missed, and applying them took the shipped path
from 1.66 / 2.07 / 1.56 to the 1.62 / 2.02 / 1.54 in §10.1 — one to three percent, and the whole of
the difference is on the `off` column that ships.

- **Two payloads, not one.** *Tailoring payload types to invoked shaders*: "anyhit requires a
  smaller payload than closesthit, because anyhit is only used for simple alpha testing... the
  application should use different actual payload types". The trace was handing the any-hit shader
  the hundred and four bytes the closest-hit shaders fill in. It now carries one word, and the
  shading payload is named only by the execute.
- **Live state is what is defined before the call and read after it.** The launch kept the ray's
  origin and direction and the camera's cone across the sort. The cone is made after it now, and the
  ray is read back off the hit object, which holds it anyway — the whitepaper's own "inspect the
  fields of the resulting HitObject" pattern.
- **The fused calls, where the data allows.** Khronos's guidance is to start with the whole of
  trace, reorder and execute as one call. The hint is read off the instance the traversal landed on,
  so only `hit`, which carries no hint, can be fully fused; `both` fuses the reorder and the execute.
  Measured on its own this moved nothing, which §10.2 reads as the cost being the sort rather than
  the call.

One thing the sources ask for that this does not do: **read the hint off the shader table rather
than off the instance row.** The whitepaper's coherence-hint example peeks at material flags stored
as root constants in the shader record, which the hit object can read directly — where
`reorderFlags` reads an instance row and a material row per pixel. This pipeline has no shader
record data at all, so it would mean giving every hit group a local root table and writing the flags
into it at build time. That is a change to the shader binding table's shape, and it is worth doing
only if a reorder ever pays here.

## 11. Sources

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
