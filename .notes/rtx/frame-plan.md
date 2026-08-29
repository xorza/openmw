# The frame path: six defects, three fixed

Everything here is about what happens between `placeScene` and the picture on the screen. Five of
the six were found while looking for something else, and four are already reproduced headlessly.
The measurements are on this box, release, `--validation=false`.

## 1. One acquire semaphore serves every acquire — **fixed**

`Presenter` held a single `mAcquired`. `vkAcquireNextImageKHR` may not be handed a semaphore that
still carries a pending operation, and the acquire's own signal stays pending until the blit that
waits it has run. The blit is submitted at once and does not run at once, so a caller that gets
ahead of the presentation engine hands the layers
`VUID-vkAcquireNextImageKHR-semaphore-01779` and the device an undefined wait.

**Why only `view` ever hit it.** It is the one caller that does not wait its own frame out — see §3.
`bench` is headless unless asked for a window, and even windowed it serialises, so its previous blit
had always finished.

**The fix.** One semaphore per swapchain image, taken in turn and never keyed on the image, which is
what an acquire returns rather than takes. A slot comes free when the fence of the blit that took it
signals, so the fence is kept beside it. Remade with the swapchain, because a suboptimal acquire
leaves a signal that only a destroy clears.

## 2. `--accumulate` has never accumulated

Measured on the Balmora mages' guild, dumping linear radiance:

| frames | linear mean | mean × N | neighbour \|diff\| ÷ mean |
| --- | --- | --- | --- |
| 1 | 0.00500 | 0.00500 | 0.457 |
| 8 | 0.00062 | 0.00497 | 0.457 |
| 512 | 0.00001 | 0.00498 | 0.456 |

The mean falls as exactly 1/N, and the frame times N carries **the same noise at 512 frames as at
one**. So the output is one frame divided by N. Nothing is averaged.

**Root cause.** `VulkanRenderer::placeScene` ends with `mHistory.reset()` under the comment *"a frame
whose instances moved is not one the last frame reprojects onto"*. That sentence is about
reprojection. `mHistory` is not a reprojection buffer — it is the composite's running sum, the whole
of the reference mode. The reprojection history is `mHistoryStale`, `mPreviousCamera` and
`AccumulatePass`'s own image pairs, and the line touches none of them. **Two different things are
called history**, and a comment about one landed on the other. Anything with a moving actor in it —
which is every shot, because the actors animate — resets the sum every frame.

**The fix.** Delete the line, and take the name away so it cannot happen again: `mHistory` becomes
`mSum`, `CompositePass`'s `history` parameter and `mNoHistory` follow, and the shader's binding is
`sum`. `CompositePass::record` already calls its local `sum`, which is the tell.

**What it invalidates.** Every convergence claim measured against an `--accumulate` reference. The
step 6 comparison in `performance-plan.md` is one of them and has to be taken again.

## 3. Two frames in flight never engaged — **fixed**

`bench --suite=streaming`, warm:

```
              median      mean       p95       p99      best     worst
  frame ms     12.07     21.86     63.81    245.93      6.14    350.14
  wait ms       4.46      4.57      6.81      7.63      0.00      8.54
  gpu ms    upscale 2.08  trace 1.49  tlas 0.21  waves 0.20  ...   (≈4.3 total)
```

The CPU stands still for 4.46 ms of every 12.07 ms frame, and the GPU frame is 4.3 ms. If the CPU
were a frame ahead the wait would be nought, because 12 ms of walking covers 4.3 ms of tracing.

**Root cause.** `finishFrame` waits `frameSlot(mFinished)` whenever `mFinished != mFrame`. Every
caller calls it in the same statement after `renderFrame`, so `mFinished` catches `mFrame` on every
iteration and the frame it waits is the one just submitted. The ring caps at two and never holds
two. `sFrameSlots = 2`, `RowDebt`, the second copy of every table and the graveyard per slot are all
paid for and none of it is used.

`renderer.hpp` already states the intended split: *"Called straight after `renderFrame` it waits the
frame out, which is a screenshot and a test; called after the next frame's walk it usually finds the
fence already signalled, which is a game."* No caller does the second thing.

**The fix.** Move the call, do not change `finishFrame`. The wait now sits **before** the submit in
`bench` and in `RtxRenderer::traceWorld`, so the frame it waits is the one behind. `shot` and
`verify` keep it after, which is what the contract already says a still wants.

**Measured after, same suite, warm:**

```
              median      wait     fps
  before      12.07 ms   4.46 ms   82.9
  after        6.70 ms   0.43 ms  149.4
```

**And it opened nothing.** 1200 pipelined frames — one interior, one exterior — under
`--sync-validation` report **zero hazards**. The barriers `SceneAcceleration` and the frame's
discards already carry are enough for a genuinely overlapped frame, which is what the step was
holding back for.

## 4. Every cell crossing threw a frame away — **fixed**

`bench --suite=streaming`: 19 crossings, 19 calls to `closeFrame`, and 19 frames whose primary hit
count is exactly nought.

```
zero-hit frames: 61 106 141 154 202 220 250 298 299 345 379 393 441 458 489 537 585 617 632
```

**Root cause.** `SceneUploader::hand` reaches `placeScene` twice inside one game frame on a crossing.
The second placement finds `mPlaced` already set, so it calls `closeFrame`, which submits an empty
command buffer to get a fence on the placement in front of it. That empty frame is a frame as far as
the ring is concerned: it takes a slot, it advances `mFrame` by one more than the caller drew, and
`finishFrame` hands its result — nought hits, an empty GPU breakdown — back to the caller as though
it were the frame just traced. Every crossing's row in a bench report is that.

**The fix.** One command buffer per placement rather than one per frame. The frame had a single
`mPlaceCommands`, so a second placement could only record over a submit in flight — hence the close.
A frame now keeps a small vector of them, grown to the busiest frame so far and never freed, and the
trace's fence covers every placement before it on the queue. `closeFrame` and `Frame::mPlaced` are
gone with it.

**After:** nineteen crossings, and not one zero-hit frame. It costs nothing — 15.06 ms a frame
before against 15.35 after, back to back.

**The contract it changes** is stated in `frames.cpp`: several placements before a trace are one
frame, and the trace reads the last of them. The ring counts frames the caller asked for.

## 5. Terrain that blinks, and body parts in the air — **two causes fixed, one left**

**Reproduced headlessly.** `bench --suite=streaming --albedo --exposure=1 --upscale=off`: 97 of 600
frames swing more than 40% in mean brightness. Frame 289 puts an enormous terrain chunk directly on
the camera; 288 and 290, 103 units either side, are open water. Rendered on their own with `shot`,
those two cameras give linear means of 0.01576 and 0.01502 — the content is the same, so the
streaming path is what differs.

**Ruled out, each by measurement:** the lighting and the fog (`--albedo` alone still swings), the
exposure (`--exposure=1`), the upscaler (`--upscale=off`), the composite bake queue (collecting none
changes nothing), the specialized kernel (one variant across all 600 frames), `outgrow`'s doubling,
and §3 — the old serial order swings 142 times.

**It is the second table copy.** Forcing both frames onto one copy takes 97 swings to 5.

**Cause A — the harness advanced the epoch on the wrong side of the hand-over.**
`SceneExtractor::advance` calls `SceneDesc::advancePlacement`, which swaps `mMoved` into `mSettled`.
`StagedWorld` called it at the *end* of a walk, so by the time `placeScene` ran `getMoved()` was
already empty and the acceleration owed its copies nothing. The game advances after its trace, which
is the same instant as the head of the next walk — and that is where it now sits.

**Cause A applies twice.** `StagedWorld::mirror` was one of the two walks the harness has;
`PosedActors::advanceTo` — the path every scene with actors in it takes — advanced on the same wrong
side, and that is the one an NPC's limbs go through.

**Cause B — the two tables disagreed about what a copy owes.** `SceneBuffers::place` owed
`getSettled()` and `getMoved()`; `SceneAcceleration::place` owed only `getMoved()`. A frame walked
twice — a crossing, or the game's precipitation subtree beside its world — settles the first walk's
moves before the placement after the second, so a copy reading only `getMoved` never learns of them.

**And the early return asked the scene rather than the copy.** `SceneAcceleration::place` skipped
the top level when `getMoved()` was empty, which is no longer the same question as whether this
copy's rows are about to change — a copy carrying a debt from an earlier frame would keep it and be
traced stale. It now asks the debt. A standing camera still returns early: `tlas` stays at 0.2 ms.

**Where it stands:** 97 swings to 26, and frames 287–291 read 34.8, 33.1, 29.7, 27.9, 25.9 — a
smooth descent where they read 127, 127, 42, 125, 125. One copy still gives 5, so something is
outstanding; 19 of the 26 are §4's crossings, which is the next step anyway.

## 6. `island-crossing` differs from itself between runs — not yet root-caused

One view of sixteen, worst 4 of 255 on 0.01% of the pixels, release, upscaling off. Everything else
is byte-identical between runs of one build.

**Where to look.** It is the streaming view, so the suspects are ordering rather than arithmetic:
what order chunks arrive in, what order slots are taken back and reused, and whether a texture keeps
its index across a run. §4 changes what a crossing does, so this is worth re-measuring after §4 and
before hunting it.

## Order

1. §1, done.
2. §2, done.
3. §3, done — brought forward, because the detector §5 needs is a pipelined `bench`, and the
   sync-validation run said it was safe to turn on.
4. §5 — two causes found and fixed, and the mechanism behind them replaced. Seven swings of the
   original ninety-seven are left once §4's nineteen come out.
5. §4, done.
6. §6 — re-measure now that §4 is out, then hunt.

**Do not give `view` the `finishFrame` its siblings have.** It is the only path that runs the ring
at its cap, which is where §1 was found and where §5 still lives.
