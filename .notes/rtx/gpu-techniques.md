# What the field says about a ray-traced frame, and what this fork does

Read in September 2026, against the measurements in `.notes/rtx/gpu-performance.md`. Every section
says what the sources recommend, what this tree already does, and what is left.

**A published figure is not a measurement of this renderer.** The numbers other people quote are
their scene, their hardware and their frame. Each one below is paired with what this fork measured
on an RTX 4090 Laptop, and where the two disagree the disagreement is the finding.

**The short answer is that the tree has read this literature already.** The build flags, the
compaction, the skybox, the two payloads and the shadow ray's early exit are all here, several of
them with a measurement written beside them that this reading only re-confirms. Two things are
open: **there is one queue**, and **a shipping title compacts the structures this tree decided not
to**. Two of the field's headline techniques were measured here and removed — Shader Execution
Reordering and opacity micromaps — and Findings 3 and 4 of `.notes/rtx/gpu-performance.md` hold
those readings.

## Acceleration structures — this fork already follows the guidance

NVIDIA's guidance is: `PREFER_FAST_TRACE` for the top level and for static bottom levels, rebuild the
top level every frame rather than update it, group geometries into one bottom level, compact what is
built once, and keep a skybox out of the top level because its box overlaps everything.

This tree does all of it. `SceneAcceleration` and `BottomLevelStore` build with
`PREFER_FAST_TRACE`, and add `ALLOW_UPDATE` for what deforms and `ALLOW_COMPACTION` for what does
not. The sky is not in the scene at all — a ray that reaches it has missed everything and the
renderer draws its own.

**So the one item on the list this fork had open is closed by the sources rather than by a
measurement.** `gpu-performance.md` asked whether a top-level refit would beat the 0.24 ms rebuild.
The guidance says to rebuild it every frame whatever the scene did, "making the TLAS as high quality
as possible regardless of the movement occurring in the scene". That item comes off the list.

**And one place where a source contradicts a decision this tree made.** `BottomLevelStore` asks for
compaction on what stands still and deliberately withholds it from what deforms: "A mesh that refits
is left out: a refit writes back into the slack." Indiana Jones does the opposite. It compacts its
*dynamic* bottom levels — built `PREFER_FAST_BUILD | ALLOW_UPDATE | ALLOW_COMPACTION`, then refitted
and never rebuilt — and took its vegetation from 1027 MB to 606 MB, a 41 per cent saving, with
individual structures halving.

The two are not obviously both right. The source's constraint is that a compacted structure must
never be rebuilt afterwards, only refitted, which is a property a caller can hold to. What this tree
states as the obstacle is that a refit writes into slack the compaction removed. **Whichever is
right, this is the one item where a shipping title did what this tree decided against, and said how**
— so it is worth reading the specification on updating a compacted structure before either is
believed.

## Async compute — the one structural gap

Every source says the same thing. NVIDIA: "Move AS management (build/update) to an async compute
queue, which pairs well with graphics workloads and in many cases hides the cost almost completely."
Khronos says to overlap the build with the rest of the frame.

**This fork has one queue.** `Device::getQueue` returns a single handle and `getQueueFamily` a single
family, so the trace, the placement and the bottom-level builds are one serial stream.

What that costs is measured. Over the island route the device spends **0.69 ms a frame on the
bottom-level build**, and it lands on the frames a cell ring arrives on — the same frames whose p99
is 49 ms and whose worst is 83. On a standing camera it costs nothing, because nothing is built.

**The caveat the sources add is about hardware older than Ampere**, where overlapping compute on the
graphics queue with a dedicated async queue can leave gaps. This fork's floor is Turing, so a second
queue would need a measurement on the floor rather than only on this card.

## Ray Reconstruction — the press figure and the measurement answer different questions

The reviews say Ray Reconstruction has "essentially no performance cost" on 40- and 50-series cards.
This fork measures it as the largest zone in every frame: 2.27 to 2.57 ms at 1920×1080 out, and 5.26
to 5.98 at the 4K target.

**Both are right, because they compare against different things.** The reviews compare a game with
Ray Reconstruction against the same game with its own denoiser stack, which Ray Reconstruction
replaces. The zone measures it against nothing.

The comparison the reviews make is one this fork can make too, and it did. At a fixed 1920×1080
render extent, Ray Reconstruction costs **5.38 ms** and this fork's own wavelet costs **1.99** —
1.65 for the filter and 0.34 for the accumulator. **Ray Reconstruction is 2.7 times the price of the
denoiser it replaces here**, at the same extent, on this card.

That is not an argument against it. It is the size of the decision, and `AGENTS.md` already made it:
image quality is not traded for simplicity or convenience. What follows from the number is narrower:
**the only knob on the largest cost in the frame is which extent to trace**, and the target already
names one that fits.

## Barriers — twenty-one places name every stage

NVIDIA's barrier guidance is to batch them, to keep the source stage as early and the destination as
late as the dependency really needs, and not to reach for `ALL_COMMANDS` "just for the sake of it",
because a redundant flag triggers a redundant flush.

This tree records **47 pipeline barriers a frame**, and **21 sites name
`VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT`** in a barrier rather than in a timestamp, against 79 that
name the compute stage and 18 the ray-tracing stage.

**Whether any of the 21 is redundant is not established.** The host cost is 205 ns each, which is
9.6 microseconds a frame and nothing. What a broad stage costs is on the device, in a flush the
dependency did not need, and no instrument in this tree can see it. What would show it is a
timestamp either side of a suspect barrier, or the pass moved and re-measured.

## Payload and ray flags — already done the way the sources ask

The guidance is to keep the payload small because it consumes registers that hit shaders would
otherwise have, and to give traversal a smaller payload than the shader that resolves the hit.

`visibility.rgen` declares exactly two: a full `VisibilityPayload` for the shader that runs on a hit,
and a single `uint` for the traversal an any-hit shader reaches. The comment beside it gives the
reason the sources give.

The other item on the list is done too. The sources call
`gl_RayFlagsTerminateOnFirstHitEXT` on shadow rays the simple and efficient optimisation, and
`lightThrough` in `lib/traversal.glsl` opens its query with exactly that — with a note on why a
translucent candidate does not cost it, since such a hit is never confirmed and traversal keeps the
early out for the first thing that stops the ray.

## What to check next in this tree, in order

1. **Price a second queue for the acceleration-structure work.** 0.69 ms a frame of bottom-level
   builds on the island route's arrival frames, and every source says it can be hidden almost
   completely. Turing is the floor, so the measurement has to be taken there too.
2. **Settle whether a refitted structure can be compacted.** A shipping title says yes and took
   41 per cent of its vegetation memory back. This tree says no in a comment. One of the two is
   wrong, and the specification says which.
3. **Read the twenty-one broad barriers.** Not because they are known to cost anything, but because
   nothing in this tree can currently say whether they do.
4. **Leave the ray flags and the build flags where they are.** Every one of them was measured here,
   and every reading agrees with what the sources say about a frame shaped like this one.

## Sources

- [Best Practices for Using NVIDIA RTX Ray Tracing (Updated)](https://developer.nvidia.com/blog/best-practices-for-using-nvidia-rtx-ray-tracing-updated/)
- [Tips and Tricks: Ray Tracing Best Practices](https://developer.nvidia.com/blog/rtx-best-practices/)
- [Vulkan Ray Tracing Best Practices for Hybrid Rendering](https://www.khronos.org/blog/vulkan-ray-tracing-best-practices-for-hybrid-rendering)
- [Path Tracing Optimizations in Indiana Jones: Opacity MicroMaps and Compaction of Dynamic BLASs](https://developer.nvidia.com/blog/path-tracing-optimizations-in-indiana-jones-opacity-micromaps-and-compaction-of-dynamic-blass/)
- [Advanced API Performance: Barriers](https://developer.nvidia.com/blog/advanced-api-performance-barriers/)
- [NVIDIA DLSS 4.5 Ray Reconstruction Review — Performance & VRAM Usage](https://www.techpowerup.com/review/nvidia-dlss-4-5-ray-reconstruction/6.html)
