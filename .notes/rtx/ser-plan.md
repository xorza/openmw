# Shader Execution Reordering: investigation and plan

Shader Execution Reordering (SER) lets a ray generation shader ask the hardware to regroup the
invocations of a launch so that a warp holds threads about to do the same work. Ada has the
hardware for it, this machine's driver exposes it, and the trace kernel is the divergent, occupancy-
bound uber-kernel it was built for. What stands in the way is that the trace is a compute dispatch,
and a reorder can only be asked for from a ray generation shader.

This is the plan for moving the trace into one and reordering it, in two stages. Stage 1 keeps every
line of the trace's own logic and its inline ray queries, changes the launch and adds one reorder
call, and can be measured in a day. Stage 2 is the full form — hit objects and separate closest-hit
shaders per surface kind — which is also the structural answer to the register cliff the shader
review measured. Stage 2 is decided on Stage 1's numbers.

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

A probe `.rgen` written with `GL_EXT_ray_tracing`, `GL_EXT_ray_query` and
`GL_EXT_shader_invocation_reorder` — a ray query, `reorderThreadEXT(hint, bits)`, a specialization
constant and an image store — compiles with `glslc -O --target-env=vulkan1.4` and passes
`spirv-val`. The SPIR-V carries `OpCapability ShaderInvocationReorderEXT`,
`OpExtension "SPV_EXT_shader_invocation_reorder"` and `OpReorderThreadWithHintEXT`. The NV spelling
(`reorderThreadNV`, `hitObjectNV`) compiles too. In this glslang the EXT *hit object* record
functions have signatures that differ from the NV ones — `hitObjectRecordMissEXT` with the NV
argument list is rejected — which matters for Stage 2 and not for Stage 1, which uses no hit object.

Ray queries are legal in a ray generation shader, so the traversal code in `traversal.glsl` and
every caller of it move across unchanged.

## 2. Why this renderer wants it

The trace is one kernel for every pixel, and what a pixel does depends on what it hit:

- **The primary hit's kind.** Sky (14 percent of pixels at the ship at Seyda Neen), water (two more
  traversals, a shaft march and a shore ray), terrain with a layer stack (four or five masked
  texture reads), a plain textured surface, a pane the eye sees through (a second traversal), and
  a submerged eye (a column integral over the whole path).
- **The bounce.** A ray that escapes takes the sky; one that lands shades a second surface with its
  own lamp reservoir and ambient ray; far ground out of doors takes neither.
- **The lamps.** The reservoir walks whatever the light grid's cell holds, which differs by
  hundreds between a street and a hall.
- **Cutouts in traversal.** 3656 of 8544 instances at Seyda Neen are cutouts, and a candidate on
  one reads a texture inside the traversal loop of every ray that crosses it.

Every one of those is a branch a warp pays for whichever way its lanes go, and `lib/variants.glsl`
already records that the kernel is occupancy-bound: the registers one path needs are registers
every pixel does without. The shader review then measured the other face of the same fact — the
driver compiles the exterior kernels at 96 registers with spills where it chose 128 a few edits
earlier, and the trace is a tenth of a millisecond slower for it. SER sorts the divergence; Stage 2
also shrinks the live state each shader carries, which is what moves the kernel off that edge.

Today's numbers to beat, `trace` zone medians from the harness bench at 1280×720 traced:

| View | trace ms | registers |
|---|---|---|
| seyda-neen-ship | 1.67 | 96, spilling |
| seyda-neen-ship-dawn | 2.15 | 96, spilling |
| balmora-mages-guild | 1.57 | 96, spilling |

`VISIBILITY_STRIP`, the hand-written workgroup permutation in `visibility.comp`, is worth 0.2 ms at
the ship by the measurement written beside it. It is a coherence trick a launch order cannot carry,
and SER is the mechanism it stood in for.

## 3. What a reorder costs and needs

- **Ray generation stage only.** `reorderThreadEXT` is defined for that stage and no other, which is
  the whole reason the launch has to change. Inline ray queries stay.
- **Live state is what a reorder moves.** The hardware packs each thread's live registers into
  memory, regroups the threads across the chip, and unpacks. The call's cost is proportional to
  what is live at it, so it goes where the state is smallest: after a traversal has answered and
  before anything is resolved off the answer.
- **One call per launch, in uniform control flow.** Every invocation reaches the call; the hint is
  what differs. A second call is measured on its own and kept only if it pays.
- **The hint.** `reorderThreadEXT(uint hint, uint bits)` sorts on the low `bits` of `hint`, most
  significant bit first. The kind of work about to happen goes in the top bits.
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

## 4. Stage 1: the trace as a ray generation shader with one reorder

### 4.1 Device

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
form and the one the layers are current for.

### 4.2 Pipeline

A `TracePipeline` beside `ComputePipeline`, with the same shape — set-0 layout, pipeline layout and
pipeline as one object, the push-descriptor set at zero, specialization words by index, statistics
captured at creation and reported through `Device::reportPipeline`. The layout creation the two
share is lifted into one function both call. What is new is the shader binding table: one
device-local buffer per pipeline, `SHADER_BINDING_TABLE` and `SHADER_DEVICE_ADDRESS` usage, the
group handle written at offset zero, and the three `VkStridedDeviceAddressRegionKHR` the launch
takes kept on the object.

`VisibilityPass::compileEvery` makes sixteen of these in place of sixteen compute pipelines, on the
same threads and the same cache. The fog volume's eight stay compute.

### 4.3 Shader

`visibility.comp` becomes `visibility.rgen`, listed in `RTX_SHADERS` under that name; glslc reads
the stage off the extension. The differences:

- `gl_LaunchIDEXT.xy` is the pixel; `gl_LaunchSizeEXT` is the frame. No workgroup, so no bounds
  test, no `VISIBILITY_WORKGROUP` and no `stripedWorkgroup`. The constant leaves `visibility.h`
  with the dispatch that read it.
- `#extension GL_EXT_ray_tracing` and `GL_EXT_shader_invocation_reorder` join the list.
- `trace()` in `traversal.glsl` splits into what it already is inside: `traverse()`, which runs the
  query and returns a `Hit` — hit or not, instance, primitive, barycentrics, distance — and
  `resolve()`, which turns a `Hit` into a `Surface`. Every present caller of `trace()` calls the
  pair; the primary ray in `main` calls `traverse()`, reorders, then `resolve()`.
- The reorder, once, between the two:

  ```glsl
  const Hit hit = traverse(origin, direction, 0.0, MASK_SOLID | MASK_WATER | MASK_FIRST_PERSON);
  reorderThreadEXT(reorderHint(hit, submerged), REORDER_BITS);
  Surface surface = resolve(hit, ...);
  ```

  `reorderHint` reads one instance row and one material row — what `resolve` reads first anyway —
  and packs, most significant first: the kind of shading ahead (miss, water, terrain with a stack,
  surface), whether the eye sees through it, whether the eye is under water, and whether the
  material is a cutout. Five bits. The pane case re-traverses after the reorder, which is the rare
  path and the right side of the call for it.
- `REORDER_BITS` is a specialization constant. Nought compiles the call away, which is the A/B that
  separates the launch change from the reorder itself; the harness takes it on the command line.
- Everything else is untouched: the sky, the water, the fog, the sprites, the bounce, the channel
  writes, the hit counter.

### 4.4 Recording

`VisibilityPass::record`: bind point `VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR` for the pipeline,
the pushed set and the three bound sets; `vkCmdTraceRaysKHR(width, height, 1)` in place of the
dispatch. `writeConstants`' second barrier and the fog volume's `handOver` name the ray tracing
stage as their destination. The GPU timer zones are unchanged.

### 4.5 Tests and verification

- A device test beside `RtxProbeTest`: a ray generation pipeline that writes `gl_LaunchIDEXT` into
  a buffer, launched over a small grid, read back and compared. It is the shader binding table and
  the launch, asked directly.
- `openmw-rtxtool verify` against a run of the compute trace: every view byte-identical. The
  arithmetic per pixel does not change, so the picture must not; a reorder changes which lanes
  share a warp and nothing a lane computes.
- The 508 `Rtx*` tests, and a `--sync-validation` pass over the default bench suite for the
  barriers.
- Register counts read off the pipeline statistics for all sixteen variants, before and after,
  because an RT pipeline is compiled under a different allocation policy and the direction is not
  known until it is read.

### 4.6 Measurement

Alternating snapshots of the release harness, three runs each, default suite plus `exteriors` and
`interiors`, in this order:

1. Compute trace, as today.
2. Ray generation trace, `REORDER_BITS` nought. This is the launch change alone: the strip
   permutation gone, the driver's own launch order in its place, the RT pipeline's register policy.
3. Ray generation trace, `REORDER_BITS` five. This is the reorder.
4. Hint layouts against each other: kind only; kind and cutout; kind, cutout and under-water; and a
   second call after the bounce's traversal with a hint of what it found.

The number that decides is the `trace` zone median; the p99 and the worst frame are read beside it.
`air`, `upscale` and the rest are the control: none of them changes.

### 4.7 Effort

One to two days: the device and pipeline work is a morning, the shader split and the launch an
afternoon, and the rest is the barrier sweep under synchronization validation and the runs.

## 5. Stage 2: hit objects and closest-hit shaders per kind

Stage 1 sorts the threads and leaves the uber-kernel whole. The full form of SER is a pipeline in
which the traversal produces a hit object, the reorder sorts on the shader that hit object names,
and the shading runs as that shader — so a warp is not merely lanes that will take the same branch,
it is lanes running the same, smaller program. That is also what takes the register cliff away:
each closest-hit shader carries only its own live state, and the trace stops being one kernel that
has to hold the union.

The shape:

- **Ray generation** builds the primary ray, calls `hitObjectTraceRayEXT`, reorders on the hit
  object with the same hint bits, and calls `hitObjectExecuteShaderEXT`. It then takes what the
  closest-hit shader wrote — the `SurfaceResponse`, the direct light, the surface's position and
  normal — and does the bounce the same way: a second `hitObjectTraceRayEXT`, a second reorder, a
  second execute. The fog, the sprites and the channel writes stay in the ray generation shader,
  where they are already the tail of the frame.
- **Closest-hit shaders** per `MaterialKind`: surface, terrain, water; a **miss** shader for the
  sky. Each is `resolve` and `shadeSurface` for its own kind, and each carries its own registers.
  The water's own two traversals stay ray queries inside its closest-hit shader.
- **Any-hit** for cutouts, which is `candidateStops` as a stage — or, with the review's finding 2,
  opacity micromaps in the build so that only the unknown state reaches it.
- **The payload** is what the closest-hit shader hands back: the response, the radiance, the
  position, the normal, the footprint and a few flags. Under sixty-four bytes, which is what keeps
  the reorder cheap.
- **The shader binding table** gains hit groups by kind. `SceneAcceleration` writes each instance's
  `instanceShaderBindingTableRecordOffset` from its material's kind, which today is a table read
  and becomes an index the hardware follows.

Open questions Stage 2 has to answer first:

- The EXT hit-object API's exact GLSL signatures in this glslang, which differ from the NV ones the
  probe used.
- Whether the shadow rays and the lamp reservoir stay ray queries inside the closest-hit shaders or
  become their own launches. The former is the smaller step and keeps `lights.glsl` whole.
- What the DLSS guide channels cost to route through a payload rather than being written where they
  are computed.

Effort: one to two weeks, after Stage 1's numbers say it is worth it. If Stage 1's reorder buys
less than a few percent, the divergence is not where the time goes and Stage 2 is worth it only for
the register relief — which is measurable on Stage 1's own register report.

## 6. Risks

- **The RT pipeline's register policy.** NVIDIA compiles ray generation shaders under different
  limits than compute. The direction is unknown until the statistics are read; step 2 of the
  measurement isolates it.
- **The reorder's own cost.** Live state at the call is what it costs; the split of `trace()` is
  what keeps that small. If step 3 loses to step 2, the hint is wrong or the call is in the wrong
  place, and the layouts in step 4 say which.
- **Barriers.** Sixty stage references, of which perhaps twenty are about the trace. Synchronization
  validation finds the ones that are missed; a missed one is a wrong picture and not a crash.
- **Compile time.** Sixteen RT pipelines on a cold cache, in parallel, through the same pipeline
  cache. The trace took 2.8 s cold as compute; an RT pipeline is expected to be similar and the
  cache makes the second run free.
- **Validation layers.** The SDK's layers are current for the EXT extension; the NV spelling is the
  fallback only for a layer bug, and is a one-line change of spelling in the shader and the device.
- **What it does not fix.** The cutout candidate loop runs inside traversal, before any reorder can
  help; that is opacity micromaps. And the fog volume stays a compute kernel, which is fine: it was
  never the divergent one.

## 7. Acceptance

Stage 1 lands when all of these hold:

- Every `verify` view byte-identical to the compute trace.
- The 508 tests pass, and the bench suite runs clean under synchronization validation.
- The `trace` zone median is no worse at any of the default suite's three views with `REORDER_BITS`
  nought, and better at both exteriors with it set.
- The register report for every variant is in the log, and the review's `shader-review.md` gets
  the numbers.
