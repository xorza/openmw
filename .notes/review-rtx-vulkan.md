When an item is resolved, delete it from this document. Do not mark it completed or retain a resolved section.

# Vulkan RTX stability, Turing compatibility, and performance review

Reviewed 2026-09-06 against working tree based on `b033c5f021dc8507541aa1120d778a1369684905`, including the existing, uncommitted allocator changes.

**The current backend excludes RTX 2000 hardware and has independent synchronization and resource-lifetime defects.** Fix those before drawing performance conclusions. The strongest unmeasured performance opportunities are memory residency, static BLAS compaction, and reducing synchronous work during streaming and GUI rendering.

Scope: `components/rtxvulkan/`, its shader-facing API use, and relevant contracts and callers in `components/rtx/`, `components/rtxbackends/`, `components/myguirtx/`, `apps/openmw/mwrender/rtx/`, and the harness/tests. The review covered device negotiation, allocation, transfers, command submission, descriptors, acceleration structures and micromaps, pipelines/SBTs/cache, render-pass dependencies, NGX, presentation, and diagnostics. This is an API and ownership audit, not a review of all light-transport mathematics.

This was a static review with official Khronos and NVIDIA documentation. `nvidia-smi` could not communicate with the NVIDIA driver in this environment. No GPU validation, Turing execution, or performance measurements were possible. No production code was changed. “Confirmed” below means the defect follows from the inspected code and API contract; it does not mean a crash was reproduced. Recommendations are conclusions drawn from that evidence.

Priority: **P1** blocks the requested hardware or risks invalid access, corruption, or failure; **P2** affects conditional correctness, sustained resource use, diagnostics, or performance. Performance candidates have no claimed speedup.

## Shared allocator ownership is not synchronized

- [ ] **P1 · Confirmed: parallel pipeline compilation races inside the memory allocator.**

  Evidence: [visibilitypass.cpp:203](/home/xxorza/Projects/openmw/components/rtxvulkan/visibilitypass.cpp:203) launches concurrent compilation workers. Each ray-tracing pipeline creates its SBT through `Buffer::hostWritten` in [tracepipeline.cpp:141](/home/xxorza/Projects/openmw/components/rtxvulkan/tracepipeline.cpp:141). That reaches the same `Device::getMemory()` allocator. [memory.cpp:136](/home/xxorza/Projects/openmw/components/rtxvulkan/memory.cpp:136) mutates both `mBlocks` and each block's free-range allocator without synchronization; `give()` and `getBytes()` are also unprotected.

  Concurrent allocations can corrupt free ranges, overlap live allocations, or invalidate references when the block vector grows. The mutex in `compileEvery` protects only the captured exception. Vulkan's internally synchronized pipeline cache does not protect this C++ bookkeeping.

  Synchronize the allocator's shared state, including release and observation, or allocate SBT storage outside the compilation workers. Preserve parallel driver compilation. Verify concurrent allocate/write/release with disjoint-range assertions and ThreadSanitizer, followed by repeated cold pipeline creation under Vulkan validation. Khronos distinguishes application-owned synchronization from driver synchronization in its [threading guide](https://docs.vulkan.org/guide/latest/threading.html).

## The device baseline assumes Ada capabilities

- [ ] **P1 · Confirmed compatibility blocker: SER and opacity micromaps are mandatory, including hardware reordering.**

  Evidence: [requirements.cpp:18](/home/xxorza/Projects/openmw/components/rtxvulkan/requirements.cpp:18) requires invocation reorder and opacity micromap extensions/features. [physicaldevice.cpp:127](/home/xxorza/Projects/openmw/components/rtxvulkan/physicaldevice.cpp:127) additionally rejects any reordering hint other than `REORDER`. The ray-tracing pipeline unconditionally enables opacity micromaps, and its shader path uses hit objects even when `REORDER_OFF` is selected: [reorder.glsl:105](/home/xxorza/Projects/openmw/components/rtxvulkan/shaders/lib/reorder.glsl:105).

  Turing cannot satisfy the hardware-SER policy. NVIDIA describes hardware SER as an Ada addition, with earlier architectures executing reordering as a no-op in its [SER whitepaper](https://developer.nvidia.com/sites/default/files/akamai/gameworks/ser-whitepaper.pdf). That document's compatibility example uses NVAPI; it is not proof that every Turing Vulkan driver exposes the EXT API. Opacity micromap acceleration is also an Ada-generation feature. See [NVIDIA's micro-mesh overview](https://developer.nvidia.com/rtx/ray-tracing/micro-mesh) and the [Vulkan reordering-mode definition](https://docs.vulkan.org/refpages/latest/refpages/source/VkRayTracingInvocationReorderModeNV.html).

  Establish a complete Turing path with the same rendering semantics: negotiate SER/OMM as accelerations, retain ordinary alpha-tested traversal when OMM is unavailable, and support ordinary KHR ray tracing when the hit-object extension is unavailable. Existing any-hit/candidate alpha evaluation supplies relevant logic; merely deleting the device checks is insufficient. Feature chains, function loading, pipeline flags, AS instance flags, shader capabilities, and micromap construction must agree on the selected capabilities. Unsupported requirements that remain essential must still fail by name.

  Verify on an actual RTX 2060-class device and on Ada, with opaque geometry, foliage, water, particles, and animated actors. Record extension versions and features from the production driver used. Force the Turing capability selection on Ada for differential image checks, then repeat on Turing itself.

- [ ] **P1 · Confirmed portability gap: host-writable VRAM is required without a suitable capacity policy and mistaken for proof of Resizable BAR.**

  Evidence: [physicaldevice.cpp:73](/home/xxorza/Projects/openmw/components/rtxvulkan/physicaldevice.cpp:73) tests only memory-property bits. The test can succeed for a small host-visible device-local heap; it does not establish that the whole VRAM allocation is CPU-accessible. Its rejection message also diagnoses firmware without evidence. [buffer.cpp:15](/home/xxorza/Projects/openmw/components/rtxvulkan/buffer.cpp:15) requires this memory for `hostWritten`, including geometry blocks, scene tables, SBTs, and even `uploadBuffer`'s temporary copy source in [commands.cpp:250](/home/xxorza/Projects/openmw/components/rtxvulkan/commands.cpp:250).

  A small aperture can pass startup and exhaust during scene upload while substantial ordinary device-local memory remains. The allocator now sizes blocks from the selected heap, but smaller blocks do not increase that heap's capacity. RTX 2000 support must work without assuming the desktop Resizable BAR configuration described for RTX 30 cards in [NVIDIA's support article](https://www.nvidia.com/en-gb/geforce/news/geforce-rtx-30-series-resizable-bar-support/).

  Make persistent host-visible staging into device-local resources a supported baseline. Use direct mapped VRAM where the actual heap budget and measurements justify it. Select upload and CPU-readback memory separately; property bits describe access, not performance or available capacity. Verify with a restricted host-visible device-local heap and scene data exceeding that heap. Khronos describes the relevant [memory-allocation model](https://docs.vulkan.org/guide/latest/memory_allocation.html).

**Vulkan 1.4 itself is not a Turing blocker.** NVIDIA lists RTX 2060 through RTX 2080 Ti under Vulkan 1.4 support. Keep the version if the actual remaining feature requirements warrant it. DLSS Ray Reconstruction is also not intrinsically Ada-only: the SDK's RR guide includes RTX 20-series memory/performance entries. These facts do not establish that this backend's full extension list works on a particular installed driver. Sources: [NVIDIA Vulkan driver support](https://developer.nvidia.com/vulkan-driver), [NVIDIA DLSS RR integration guide](https://github.com/NVIDIA/DLSS/blob/v310.7.0/doc/DLSS-RR%20Integration%20Guide.pdf).

## Dependencies do not cover every actual consumer

- [ ] **P1 · Confirmed: image handovers omit sampled-image reads.**

  Evidence: [gbuffer.cpp:238](/home/xxorza/Projects/openmw/components/rtxvulkan/gbuffer.cpp:238) exposes ray-tracing writes only to compute `SHADER_STORAGE_READ`. The G-buffer guides are subsequently sampled by NGX; [dlsspass.cpp:57](/home/xxorza/Projects/openmw/components/rtxvulkan/dlsspass.cpp:57) explicitly requires sampled-image usage. Separately, [vulkanrenderer.cpp:1175](/home/xxorza/Projects/openmw/components/rtxvulkan/vulkanrenderer.cpp:1175) exposes NGX output only to `SHADER_STORAGE_READ`, although the immediately following bloom pass binds it as a combined image sampler in [bloompass.cpp:97](/home/xxorza/Projects/openmw/components/rtxvulkan/bloompass.cpp:97).

  Storage-read visibility does not include sampled reads. The later barrier on `mColour` covers that image, not the separate guide images. `GENERAL` layout does not supply the missing memory dependency. These paths can consume stale image data without a deterministic failure on every GPU.

  Include `SHADER_SAMPLED_READ` for sampled consumers and use destination stages that cover NGX's integration contract. Preserve narrower scopes for consumers whose accesses are known. Check RR on/off and bloom on the upscaled output using synchronization validation and deterministic channel comparisons. See Khronos's [synchronization examples](https://docs.vulkan.org/guide/latest/synchronization_examples.html) and NVIDIA's [DLSS programming guide](https://github.com/NVIDIA/DLSS/blob/v310.7.0/doc/DLSS_Programming_Guide_Release.pdf).

- [ ] **P1 · Confirmed: image readback has no transfer-write-to-host-read memory dependency.**

  Evidence: [image.cpp:231](/home/xxorza/Projects/openmw/components/rtxvulkan/image.cpp:231) copies into a staging buffer, waits for the submission, and reads the mapped buffer with `memcpy`. Its two barriers concern the source image. Neither covers the destination buffer's transfer writes becoming visible to host reads.

  Add a destination-buffer or global dependency from `COPY / TRANSFER_WRITE` to `HOST / HOST_READ` before the submission completes. Keep the fence wait. Host coherence removes the invalidate requirement; it does not replace that dependency. This affects screenshots, channel readback, and GUI/map readback. Exercise known texel patterns through every readback format, including after prior GPU work. Khronos's [CPU readback example](https://docs.vulkan.org/guide/latest/synchronization_examples.html) explicitly includes the barrier, completion wait, and conditional invalidation.

- [ ] **P1 · Confirmed for a supported call sequence: rendering twice from one placement races sprite buffers.**

  Evidence: [renderer.hpp:636](/home/xxorza/Projects/openmw/components/rtx/renderer.hpp:636) permits asynchronous rendering with two frames in flight. The implementation explicitly supports frames without a new placement. Nevertheless, [vulkanrenderer.cpp:1108](/home/xxorza/Projects/openmw/components/rtxvulkan/vulkanrenderer.cpp:1108) bins into `mWorldSlot`, and [scenebuffers.cpp:237](/home/xxorza/Projects/openmw/components/rtxvulkan/scenebuffers.cpp:237) rewrites mapped sprite data and reads the GPU-written bin report. The `mReadBy` wait occurs only in `placeScene`, not before those accesses in `renderFrame`.

  Sequence: place once, render A, render B before finishing A. Both renders use the same placement slot. B can overwrite data A is reading and read a report A is still writing. A command-buffer ring slot becoming available does not establish ownership of this different resource slot.

  Give camera-dependent sprite/bin resources frame ownership independent of placement ownership, or explicitly wait for their last reader/writer before every host access. Prefer preserving the two-frame overlap the interface promises. Verify two consecutive renders with no placement, changing camera/sun and nonempty sprites, against a serial reference. Synchronization validation should be supplemented by explicit ownership checks because host mapped-memory races are not all observable by the layers. See [Vulkan synchronization](https://docs.vulkan.org/spec/latest/chapters/synchronization.html).

- [ ] **P1 · Confirmed when sea state changes with work pending: wave buffers are destroyed before their last use finishes.**

  Evidence: [wavepass.cpp:137](/home/xxorza/Projects/openmw/components/rtxvulkan/wavepass.cpp:137) assigns new amplitude/frequency buffers directly over the existing buffers, then flushes the upload batch. [vulkanrenderer.cpp:733](/home/xxorza/Projects/openmw/components/rtxvulkan/vulkanrenderer.cpp:733) calls this before the placement-resource wait. Old buffers can therefore be destroyed while the previous frame's wave dispatch still reads them. Waiting after replacement is too late.

  Retire old spectra with the submission that last used them, or establish completion before replacement. Integrate new uploads into the ordered submission/resource-retirement path. The present game caller supplies `SeaState{}` in [worldmirror.cpp:145](/home/xxorza/Projects/openmw/apps/openmw/mwrender/rtx/worldmirror.cpp:145), so this is a backend API defect masked by that caller, not evidence of a current weather-triggered crash. Verify alternating distinct sea states with two frames pending. Vulkan requires submitted buffer users to complete before [buffer destruction](https://docs.vulkan.org/refpages/latest/refpages/source/vkDestroyBuffer.html).

## Failure paths lose ownership before work is safe

- [ ] **P1 · Confirmed: batch destruction submits commands from failed construction.**

  Evidence: [commands.cpp:168](/home/xxorza/Projects/openmw/components/rtxvulkan/commands.cpp:168) calls `flush()` from `Batch::~Batch`, including during exception unwinding. A concrete path is [texture.cpp:162](/home/xxorza/Projects/openmw/components/rtxvulkan/texture.cpp:162): the primary image upload is recorded, then constructing the shading image can fail. The partially constructed texture releases its primary image; the enclosing batch can subsequently submit the command buffer that names it.

  Make submission an explicit successful operation. An abandoned batch must discard unsubmitted commands and release staging/resources in a safe order. Once a batch has been submitted, retain its referenced resources until completion; a timeout is not proof of completion. Verify injected failures after recording the first upload and before completing the second, with object-lifetime validation enabled. The relevant lifetime rules are in [Vulkan fundamentals](https://docs.vulkan.org/spec/latest/chapters/fundamentals.html).

- [ ] **P1 · Confirmed: constructor failures leak Vulkan handles, and device-construction cleanup can destroy a parent before its child.**

  Evidence: [image.cpp:91](/home/xxorza/Projects/openmw/components/rtxvulkan/image.cpp:91) creates a raw image before allocating memory and creating views. If a later step throws, `Image::~Image` does not run, leaving raw handles unreleased. Similar multi-step raw-handle construction exists in command-pool/fence, descriptor-pool, and wave-sampler initialization. More directly, [device.cpp:167](/home/xxorza/Projects/openmw/components/rtxvulkan/device.cpp:167) constructs `mPipelineCache` before allocating `mMemory`; its catch destroys the device first. If the latter allocation throws, unwinding subsequently runs the pipeline-cache destructor against an already destroyed device.

  Adopt existing owned-handle wrappers immediately after successful creation, and destroy all constructed children before their device on every exit. Test failures at successive allocation/creation steps, especially low-memory startup and resizing. This matters more on smaller Turing heaps, where an allocation failure must remain a controlled failure rather than trigger additional invalid calls. See the device-child lifetime requirements for [vkDestroyDevice](https://docs.vulkan.org/refpages/latest/refpages/source/vkDestroyDevice.html).

- [ ] **P2 · Confirmed SDK ownership violation: the NGX capability parameter map is never explicitly destroyed.**

  Evidence: [dlss.cpp:142](/home/xxorza/Projects/openmw/components/rtxvulkan/dlss.cpp:142) obtains the map with `NVSDK_NGX_VULKAN_GetCapabilityParameters`. [dlss.cpp:195](/home/xxorza/Projects/openmw/components/rtxvulkan/dlss.cpp:195) only calls `Shutdown1`, with a comment claiming NGX owns the map. The SDK distinguishes this function from deprecated `GetParameters`: capability maps require `NVSDK_NGX_VULKAN_DestroyParameters`.

  Destroy the capability map before shutdown on normal and failed initialization paths. Check repeated renderer creation/destruction with allocation tracking. This is separate from feature memory, for which `DlssPass` already requests `FreeMemOnReleaseFeature`. Sources: [NGX Vulkan header](https://github.com/NVIDIA/DLSS/blob/v310.7.0/include/nvsdk_ngx_vk.h), [programming guide §6.3](https://github.com/NVIDIA/DLSS/blob/v310.7.0/doc/DLSS_Programming_Guide_Release.pdf).

## Presentation assumes capabilities and completion it has not established

- [ ] **P2 · Specification guarantee gap: queue-idle and blit fences do not prove presentation-resource retirement.**

  Evidence: [presenter.cpp:88](/home/xxorza/Projects/openmw/components/rtxvulkan/presenter.cpp:88) and `resize()` wait for the device, then destroy presentation semaphores and the swapchain. `mPresenting` fences belong to `vkQueueSubmit2`'s blit, not to the following `vkQueuePresentKHR`. No swapchain-maintenance present-fence feature is negotiated.

  Khronos explicitly documents that unextended queue/device-idle waits do not provide the presentation-completion guarantee needed here. It also notes that this common pattern often works and validation does not diagnose it. Accordingly, this is not a reproduced driver failure. Negotiate swapchain maintenance and use actual present fences to retire presentation resources; verify the supported EXT/KHR form on the target driver. Cover resize, minimize/restore, VSync changes, out-of-date returns, and shutdown with presentation pending. Source: [Khronos swapchain semaphore guide](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html).

- [ ] **P2 · Confirmed conditional invalid usage: swapchain creation ignores zero extent and required surface capability bits.**

  Evidence: [swapchain.cpp:126](/home/xxorza/Projects/openmw/components/rtxvulkan/swapchain.cpp:126) queries capabilities but always requests `TRANSFER_DST` usage and opaque composite alpha without checking their supported masks. It also creates a swapchain after accepting a surface-reported zero extent. Clamping the game's requested window size to at least one pixel does not prevent the surface itself from reporting zero while minimized.

  Suspend presentation/recreation while the surface extent is zero. Validate transfer-destination usage, composite alpha, and the chosen format's blit support before creation/use; select a supported semantically equivalent alpha mode or fail naming the unsupported operation. Exercise synthetic capability sets as well as actual minimize/restore. Relevant requirements include `imageExtent-01689`, `presentMode-01427`, and `compositeAlpha-01280` in [VkSwapchainCreateInfoKHR](https://docs.vulkan.org/refpages/latest/refpages/source/VkSwapchainCreateInfoKHR.html), plus [blit format requirements](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdBlitImage.html).

- [ ] **P2 · Confirmed conditional image error: surface-format selection knowingly accepts incorrect output encoding.**

  Evidence: [swapchain.cpp:32](/home/xxorza/Projects/openmw/components/rtxvulkan/swapchain.cpp:32) picks any matching UNORM format without checking `colorSpace`, then falls back to the first surface format with only a warning. The renderer supplies an already display-encoded image, and [presenter.cpp:264](/home/xxorza/Projects/openmw/components/rtxvulkan/presenter.cpp:264) blits it. An sRGB destination applies another encoding; an incompatible color space assigns a different meaning to the values.

  Select a format/color-space pair matching the actual output contract. If only an sRGB destination is supported, make the final conversion correct, or reject it explicitly until implemented. Never continue with a knowingly incorrect picture. Verify a gray ramp and color patches through both presentation and screenshot paths. Vulkan specifies sRGB destination conversion in [vkCmdBlitImage](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdBlitImage.html).

- [ ] **P2 · Confirmed retained allocation: each swapchain rebuild loses a set of command-buffer handles.**

  Evidence: [presenter.cpp:122](/home/xxorza/Projects/openmw/components/rtxvulkan/presenter.cpp:122) resets the command pool and then overwrites `mCommands` with another allocation. `vkResetCommandPool` resets existing command buffers; it does not free those objects. Old buffers remain allocated in the pool until presenter destruction.

  Reuse the existing buffers or explicitly free them once their submissions finish, adjusting the count when the swapchain image count changes. Verify command-buffer allocation count plateaus through repeated resize/VSync cycles. See [vkResetCommandPool](https://docs.vulkan.org/refpages/latest/refpages/source/vkResetCommandPool.html).

## Memory residency is managed by historical demand

- [ ] **P2 · Confirmed retention; workload-dependent failure risk: allocator blocks never return to the heap, and available memory is not budgeted.**

  Evidence: [memory.cpp:199](/home/xxorza/Projects/openmw/components/rtxvulkan/memory.cpp:199) releases only suballocation ranges. Blocks survive until device destruction. Pools are separated by memory type and tiling, so free memory in one cannot serve another. Allocation size is based on static heap size, and there is no `VK_EXT_memory_budget` query. Scene statistics report payload sizes rather than all reserved allocator bytes or NGX allocations.

  Repeated target sizes, scene populations, and temporary peaks can strand significant memory in empty or sparsely used blocks. Reuse mitigates this; it does not establish a bound matching a 6 GB card or the process's current budget.

  Account per heap for reserved bytes, live bytes, budget, and external allocations. Give empty blocks an explicit retirement policy that avoids a sweep on the frame path. Preflight large changes against the current budget and fail with useful resource/heap information when they cannot fit. Verify a long route with cell arrivals/departures and repeated target changes; report the settled and peak reservation, not just scene payload. Khronos defines budget as dynamic rather than physical heap size in [VkPhysicalDeviceMemoryBudgetPropertiesEXT](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceMemoryBudgetPropertiesEXT.html).

- [ ] **P2 · Unmeasured performance opportunity: static BLASes are never compacted, and their build positions remain resident.**

  Evidence: [sceneacceleration.cpp:328](/home/xxorza/Projects/openmw/components/rtxvulkan/sceneacceleration.cpp:328) requests fast trace and data access, with update enabled only for deforming meshes, but never requests compaction. No compacted-size query/copy path exists. `mPositions` retains static build inputs even though hit positions can be fetched from the acceleration structure.

  Add a static-geometry compaction stage with bounded temporary memory and fence-based retirement. Separate immutable build-only positions from positions required for skinning/refits or subsequent rebuilds, then release the former after their last actual consumer. Do not discard indices/attributes still read by shading, and verify the OMM/data-access combination on both capability paths.

  Compare AS bytes, retained build-input bytes, transient peak, load cost, and traversal cost on the same content. Compaction's benefit is content/driver dependent; it is not a guaranteed percentage improvement. Preserve the existing restriction of `ALLOW_UPDATE` to deforming meshes. NVIDIA discusses compaction and build/update choices in its [RTX ray-tracing best practices](https://developer.nvidia.com/blog/best-practices-for-using-nvidia-rtx-ray-tracing-updated/).

## Work that could overlap is submitted synchronously

- [ ] **P2 · Unmeasured performance opportunity: streaming and GUI tracing can drain the frame pipeline.**

  Evidence: [vulkanrenderer.cpp:594](/home/xxorza/Projects/openmw/components/rtxvulkan/vulkanrenderer.cpp:594) drains frames before world extension; offscreen placement uses `submitAndWait` at line 720; GUI tracing drains again at [line 1307](/home/xxorza/Projects/openmw/components/rtxvulkan/vulkanrenderer.cpp:1307) and submits synchronously at line 1327. Map and doll work can therefore introduce CPU/GPU rendezvous during play. The normal game already finishes its previous frame before the world handoff, so an additional `finishAll()` is not necessarily an additional stall on every arrival.

  Remove waits only after moving mutable descriptors, address tables, and offscreen resources under explicit submission ownership. The texture set is currently shared and is not an update-after-bind layout; deleting waits alone would introduce invalid descriptor updates. Enqueue independent GUI/upload/build work into the existing ordered submission path, preserving waits where the CPU actually needs a result. A dedicated transfer queue is a separate measurement decision, not an automatic improvement on Turing.

  Validate cell arrivals while frames and GUI traces are pending. Once content is complete, use a GPU/CPU timeline to identify actual bubbles and measure median, p99, and worst-frame time. NVIDIA recommends minimizing unnecessary synchronization and appropriately grouping submissions in [Vulkan dos and don'ts](https://developer.nvidia.com/blog/vulkan-dos-donts/).

- [ ] **P2 · Unmeasured performance opportunity: each image in a shared dependency point gets a separate barrier command.**

  Evidence: [gbuffer.cpp:230](/home/xxorza/Projects/openmw/components/rtxvulkan/gbuffer.cpp:230) and `handOver()` each loop over all channels and call `Image::transition`; that helper emits one `vkCmdPipelineBarrier2` per image. [fogvolume.cpp:194](/home/xxorza/Projects/openmw/components/rtxvulkan/fogvolume.cpp:194) has similar groups. Many resources share identical producer/consumer stages at these points.

  Build fixed-size barrier arrays and emit one dependency command per actual handover. Preserve separate commands where an intervening operation needs the dependency. After correcting the access scopes above, narrow broad stage scopes only where all consumers are known; NGX's opaque work must remain covered. Compare command-recording cost and GPU pass boundaries without adding per-frame allocations. NVIDIA recommends grouping barriers in [Vulkan dos and don'ts](https://developer.nvidia.com/blog/vulkan-dos-donts/); the size of the benefit here is unmeasured.

## Validation can report a clean result without retaining all evidence

- [ ] **P2 · Confirmed: requesting validation does not ensure validation runs.**

  Evidence: [instance.cpp:85](/home/xxorza/Projects/openmw/components/rtxvulkan/instance.cpp:85) continues after a warning when the requested layer or debug-utils extension is unavailable. The validation log then does not exist, and `takeValidationErrors()` returns an empty result. A checking harness can mistake unavailable validation for a clean pass.

  A run explicitly requiring validation must fail naming the unavailable layer/extension, or expose an explicit unavailable result that the gate treats as failure. Verify missing-layer and missing-debug-utils configurations separately from a successful validation run. This is a reporting/control-flow defect; it does not require a GPU fault to demonstrate. See [Khronos validation-layer guidance](https://docs.vulkan.org/guide/latest/validation_overview.html).

- [ ] **P2 · Confirmed in logging mode: collecting validation errors drops errors from compilation workers.**

  Evidence: [validation.cpp:46](/home/xxorza/Projects/openmw/components/rtxvulkan/validation.cpp:46) returns only messages from the caller's thread. [vulkanrenderer.cpp:1502](/home/xxorza/Projects/openmw/components/rtxvulkan/vulkanrenderer.cpp:1502) copies that subset, then clears the entire log. Pipeline creation occurs on other threads, so their errors disappear when the main thread collects results. A message arriving between the separately locked read and clear can also be lost. The default abort policy limits exposure; test/harness logging mode remains affected.

  Drain all errors for the renderer atomically under the existing mutex. If attribution is needed, retain thread metadata without filtering away failures. Verify a worker-recorded error and an error racing collection are returned exactly once. This follows directly from the log implementation and the real compilation call path.

## Cache retention discards reusable compilation work

- [ ] **P2 · Confirmed unnecessary invalidation: creating one pipeline cache deletes other valid cache variants.**

  Evidence: [pipelinecache.cpp:224](/home/xxorza/Projects/openmw/components/rtxvulkan/pipelinecache.cpp:224) removes every other regular file beginning with the RTX prefix in the cache directory. Cache names already distinguish hardware/driver/shader identity, so alternating builds or GPUs repeatedly destroys caches that would otherwise be reusable. The filename test also accepts more names than the cache writer's exact filename grammar.

  Retain a bounded set of valid variants and prune by an explicit cache policy outside rendering/startup-critical work, matching only files this cache owns. Verify A → B → A shader/device cache selection reuses A without deleting unrelated prefixed files. Measure cold and warm startup separately. Khronos explains the purpose and reuse of [pipeline caches](https://docs.vulkan.org/guide/latest/pipeline_cache.html).

## Existing decisions worth preserving

The following inspected choices agree with the relevant API model and should survive the fixes:

- Presentation wait semaphores are indexed by acquired swapchain image. That is the correct steady-state reuse pattern; the finding concerns retirement during recreation/shutdown. See [Khronos's semaphore guide](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html).
- Acceleration-structure builds use separate scratch ranges for concurrently recorded builds and order BLAS results before TLAS consumption. Only deforming meshes request update support. These match [NVIDIA's AS guidance](https://developer.nvidia.com/blog/best-practices-for-using-nvidia-rtx-ray-tracing-updated/).
- Suballocation, persistent mapping, reusable frame resources, device addresses, synchronization2, and deferred resource retirement are suitable foundations. Their ownership gaps should be repaired without replacing them with per-object allocation or a global wait on every frame.
- A shared pipeline cache without the externally-synchronized flag permits concurrent pipeline creation; it is not the allocator race above. See [VkPipelineCacheCreateFlagBits](https://docs.vulkan.org/refpages/latest/refpages/source/VkPipelineCacheCreateFlagBits.html).
- Host-coherent CPU writes completed before submission do not need gratuitous flushes. This differs from GPU-to-CPU readback. See [Khronos transfer examples](https://docs.vulkan.org/guide/latest/synchronization_examples.html).
- Not enabling `shaderStorageImageExtendedFormats` is not itself an invalid-use finding: its enablement does not change shader behavior; support establishes format guarantees. See [Vulkan feature definitions](https://docs.vulkan.org/spec/latest/chapters/features.html).

## Evidence needed before claiming stable RTX 2000 support

1. Capture a device/driver capability manifest on a real RTX 2060-class card and an Ada card: API version, extensions and revisions, features, RT/SBT/AS limits, memory types/heaps/budgets, and NGX RR availability. Use a supported production driver; the existence of a Vulkan beta driver is not a stability recommendation.
2. Run core/object-lifetime validation and synchronization validation, with missing validation treated as failure and all worker errors collected. Use GPU-assisted validation separately where supported. Also consider optional `VK_NV_ray_tracing_validation` for pipeline/SBT/shader execution issues; it complements the Khronos layers and does not cover compute ray queries. NVIDIA describes its scope in [driver-level ray-tracing validation](https://developer.nvidia.com/blog/ray-tracing-validation-at-the-driver-level/).
3. Cover cold startup, injected allocation failures, repeated renders without placement, changing sea states with work pending, cell streaming, texture replacement/removal, animated BLAS updates, GUI/map traces, readback, and shutdown. Presentation-specific checks need controlled minimize/restore and resize tests; ordinary rendering checks can use the headless harness.
4. Compare deterministic images with optional accelerations enabled/disabled, and across the Turing/Ada paths. Include alpha-tested sheets, particles, water, skinning, fog, RR, and bloom. Set explicit image-comparison tolerances according to the operation; cross-device floating-point output is not generally bit-identical.
5. After the renderer draws the complete content, measure release builds with validation disabled. Run hot-card, interleaved comparisons of identical workloads and report median, p99, worst frame, peak/settled heap use, core clock, and temperature. Prioritize residency and measured stalls before shader or queue-policy changes. No timing or 60 fps claim is established by this review.
