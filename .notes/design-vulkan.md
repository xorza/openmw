# Design: what to change structurally, and what not to build

Answers `review-vulkan.md`. Written 2026-09-06.

## Verdict on the proxy

**Do not build a Vulkan wrapper.** It would not have caught the bugs.

Of the 21 findings, a call-forwarding proxy prevents **none**. A proxy catches "you called the wrong
function". Every finding here is "you called the right function with the wrong scope, at the wrong
time, or against the wrong memory" — which a forwarding layer cannot see. `Owned<>` already covers
the one class a wrapper is good at, handle lifetime.

Two findings — the missing `SHADER_SAMPLED_READ` scopes — would be caught by a *stateful* tracker.
But this tree already has the stateful tracker that knows those rules, and knows several hundred
more: `instance.cpp:140` wires up synchronization validation with
`syncval_shader_accesses_heuristic`, which `check_rtx_checks.sh` then turns off. **The layer is
built and not gated.** Writing our own worse copy of it is the wrong answer twice.

A wrapper would also break the tree's own rule. `AGENTS.md`: *a fact about Vulkan that leaks into the
core is a bug*. A proxy is more Vulkan surface, not less.

## What the findings actually share

Five causes, and each has one answer.

| Cause | Findings | Answer |
|---|---|---|
| Capability is a hard-coded constant, not a decision | reorder hint, BAR test, `hostWritten`, micromap level, driver floor | **`DeviceProfile`**: one decided-once object, a pure function of the queried device |
| The producer guesses what the consumer will do | G-buffer → NGX, NGX → bloom, 47 hand-written transitions, barrier batching | **Consumer-declared use**: a resource carries its state; the consumer names its intent |
| Host reads of device memory are ad hoc | screenshots, hit counters, sprite bin report | **One `readBack()`**; the layers cannot see a `memcpy`, so the rule must live in the type |
| Ownership is implicit at failure and replacement points | batch destructor, device catch, wave spectra, orphaned command buffers | **Explicit submission, one retirement path** |
| Nothing accounts for device memory | no budget, blocks never freed, positions held twice | **The allocator owns residency**, and reports it |

## Numbers taken today, on the 4090

They set the sizes below.

    scene, seyda-neen-ship   4526 instances   vertex+index 73.3 MiB   120.9 MiB structures   20.9 MiB micromaps
    scene, vivec             4492 instances   vertex+index 84.7 MiB   135.5 MiB structures   25.7 MiB micromaps
    bench, island-crossing   5823 instances                          226.1 MiB structures   42.7 MiB micromaps

Against a host-visible video-memory heap of **246 MiB** on an RTX 2060 and **214 MiB** on an RTX
2080. Structures are device-local and do not compete. Geometry, every frame table and every shader
binding table do.

**And the positions are held twice.** `sceneacceleration.hpp:300` makes `mPositions` a `SlotBlocks`,
so `reserve` grows **both** copies to the whole scene, while `sceneacceleration.cpp:200` writes copy
1 only for a mesh that deforms — 138 drawables of 2800 at Seyda Neen. Roughly a third of the
host-visible working set is reserved for nothing.

Nothing reports the total. That is the first thing to fix, because until it is reported the Turing
question cannot be settled by measurement.

---

## The four things to build

### 1. `DeviceProfile` — capability as a decision

`components/rtxvulkan/deviceprofile.{hpp,cpp}`. A pure function of `DeviceProperties`,
`DeviceFeatures` and the extension list, returning what this renderer will *do* on this card:

```cpp
struct DeviceProfile
{
    bool mReorders = false;              ///< the hint says REORDER, so a sort is worth asking for
    VkDeviceSize mHostVisibleVram = 0;   ///< the largest heap the host writes straight into
    Residency mBulkGeometry;             ///< Direct or Staged
    std::uint32_t mMicromapLevel = 0;
};

DeviceProfile profileOf(const DeviceProperties&, const DeviceFeatures&, std::span<const std::string>);
std::string disqualify(const DeviceProfile&);   ///< empty where the card can run this renderer
```

`physicaldevice.cpp` stops deciding and starts reporting. `disqualify` names only genuine blockers —
a missing extension, a missing feature, a micromap level under the bake's — and never a hint or a
heap size.

**Testable without Turing.** Two fixtures written from the hardware-database reports read for the
review: the RTX 2060's three heaps with hint `NONE`, and this box's single 16 GiB host-visible heap
with hint `REORDER`. Assert the profile differs in exactly the two fields it should. That is the
"prove the parameters matter" rule applied to hardware nobody here owns.

Kills findings 1, 3 and the micromap check. ~200 lines, ~150 of test.

### 2. Residency — the allocator owns where things live

`memory.{hpp,cpp}` grows a policy rather than a wrapper.

- **Report first.** `MemoryAllocator::report()` gives reserved, live and — through
  `VK_EXT_memory_budget`, present on both Turing reports — budgeted bytes per heap. `scene` and
  `bench` print it. Everything below is judged against that number.
- **`hostWritten` becomes two answers behind one name.** `Residency::Direct` writes straight into
  video memory, as now. `Residency::Staged` puts the buffer in ordinary device-local memory and
  routes `write()` through the existing `Batch` staging. The choice is the profile's, made once.
- **Split the second position copy.** A `SlotBlocks` that reserves the whole scene twice becomes a
  compact table indexed for deforming meshes only. This is worth doing on the 4090 too — it is
  reserved bytes bought for nothing on every card.
- **Retire an empty block.** Incrementally, at most one a frame, never a sweep.

Kills findings 2 and the two P3 memory items. ~250 lines changed, ~100 of test.

### 3. Consumer-declared resource use

`Image` and `Buffer` carry their last stage, access and layout. A consumer says what it is about to
do; the barrier is derived and batched.

```cpp
enum class Use { None, TraceWrite, ComputeWrite, ComputeRead, SampledRead,
                 TransferRead, TransferWrite, HostRead, Present, Opaque };

class Barriers                       ///< a fixed-size batch, no allocation
{
    void add(const Image&, Use next);
    void add(const Buffer&, Use next);
    void flush(VkCommandBuffer);     ///< one vkCmdPipelineBarrier2
};
```

Three rules stop being retyped at 47 sites: `SampledRead` carries `SHADER_SAMPLED_READ`,
`Opaque` — which is NGX and nothing else — carries `ALL_COMMANDS`/`MEMORY_*`, and `HostRead` carries
`HOST`/`HOST_READ`.

**Sound here because recording order is submission order**: one thread records, deferred batches are
submitted ahead of the frame they were recorded before, and `GBuffer::begin` discards each frame.
Neither holds on a second queue, so a second queue would need this revisited — state that in the
type.

Kills the `SHADER_SAMPLED_READ` findings, and the 28 barrier commands a frame become 2.
~300 lines added, ~150 removed at the call sites.

### 4. `readBack()` — the one host read

**The validation layers cannot catch this class.** They see no `memcpy`. So the rule goes in the
type: `Buffer::readBack(pool, out)` records the `TRANSFER_WRITE → HOST_READ` dependency, submits,
waits and copies. `Buffer::map()` for reading goes away; `image.cpp:252`, `framering.cpp:117` and
`scenebuffers.cpp:246` all go through it.

~80 lines, and it deletes three hand-written sequences.

## The gate, which costs almost nothing

`CI/check_rtx_validation.sh`: `openmw-rtxtool check --sync-validation` over a suite that reaches an
upscaled frame, a GUI trace, a map tile, a doll and a cell arrival. Any message fails the run. Also
make a run that asked for validation and could not have it fail by name — `instance.cpp:87` warns
today — and drain the whole log rather than the calling thread's — `vulkanrenderer.cpp:1511`.

This is the highest value per line in the document. Forty lines of shell and two small fixes buy the
whole device-side barrier class, permanently.

## What is not worth building

- **A Vulkan proxy.** Above.
- **A render graph.** Passes here are a fixed sequence decided at build time, not a graph discovered
  at run time. The state tracker in §3 is the part of a graph that pays; automatic pass ordering is
  the part that does not.
- **A second code path for Turing.** `AGENTS.md` already forbids it, and nothing in the review needs
  one: the profile changes numbers and residency, not the shape of a frame.
- **Hand-written barrier validation.** The layer is better and already wired.

---

## Stages

Each stands alone and leaves the tree working.

**Stage 0 — see it. Done.** `MemoryAllocator::report` walks every block and every heap,
`VK_EXT_memory_budget` says what the driver will give, and `scene` and `bench` print the lines under
the place they belong to. `CI/check_rtx_validation.sh` drives `check`, `shot`, `map`, `doll` and
`bench` under synchronization validation. A run that asked for the layers and cannot have them now
fails by name, and a pipeline compile worker's validation errors are filed under the thread that
started it rather than lost.

**One thing the design had wrong.** A per-heap line cannot answer the Turing question on this box: a
card with resizable BAR states one video memory heap that is host-visible throughout, so every image
lands in the same heap as the geometry and the heap's own figure says nothing about either. The
report carries a `host-written` line beside the heaps, counted off the memory *type* the resource
asked for, and that is the figure that has to fit an aperture.

**The baseline, measured.** `scene` at `seyda-neen-ship`, one still cell:

    heap 0  host-visible   16376.0 MiB   reserved   991.6   live   852.7   budget 12263.5   held  1332.4   21 blocks
    heap 1  device-only    47931.1 MiB   reserved    56.0   live     0.0   budget 47931.1   held    78.8    3 blocks
    host-written                         reserved   184.0   live   158.2

And `bench --views=island-crossing --seconds=10`, twenty cell boundaries:

    host-written                         reserved   248.0   live   212.1

**184 MiB standing still, 248 MiB after a route, against 246 MiB on an RTX 2060 and 214 MiB on an
RTX 2080.** So the P1 in `review-vulkan.md` is not a risk, it is a measurement: one Seyda Neen fills
an RTX 2080's whole aperture before the player walks anywhere, and ten seconds of walking passes an
RTX 2060's. Stage 2 has its number.

**Stage 1 — `DeviceProfile`.** The type, the two synthetic fixtures, `physicaldevice.cpp` reduced to
reporting. A Turing card is accepted at the end of this, and fails on memory rather than on policy.
*Two to three days.*

**Stage 2 — residency.** Budget query, the split position copy, `Residency::Staged`, incremental
block retirement. Measure the reserved total at `island-crossing` before and after. A Turing card
runs at the end of this.
*Three to five days.*

**Stage 3 — resource use.** `Use`, `Barriers`, then the 47 call sites, one file at a time, under the
Stage 0 gate. `readBack()` lands here. Measure the frame's barrier count and recording cost.
*Three to five days.*

**Stage 4 — ownership.** Explicit `Batch::submit`, `~Batch` discards, `Graveyard::replace` for the
wave spectra, the device `catch` resets its children, the presenter frees its old command buffers,
the swapchain refuses a zero extent and checks its two masks.
*Two to three days.*

**Stage 5 — the remainder.** NGX capability parameters, pipeline-cache retention, present fences
through `VK_EXT_swapchain_maintenance1`, BLAS compaction, frame ownership for the sprite tables.
*Two days.*

## Cost

Roughly **+900 lines of production code and +400 of tests**, against ~150 lines of hand-derived
barrier arithmetic deleted. No new abstraction layer, no indirection on the frame path, and every
one of the four types is a thing this renderer already decides implicitly, written down once.

## Risks worth naming

- **A state tracker can hide a missing barrier by being conservative.** Keep `Opaque` for NGX only,
  and keep the Stage 0 gate running so the layer still has the last word.
- **`Residency::Staged` puts a copy back on the cell-arrival path** that `BlockedBuffer` was written
  to remove. It is a load-path cost, not a frame one, and only on cards that need it — but it has to
  be measured at `island-crossing`, where an arrival already costs 120 ms in the worst frame.
- **Stage 2 changes what a run reserves, so every memory figure before it is not comparable.** The
  Stage 0 baseline above is the one to compare against.
