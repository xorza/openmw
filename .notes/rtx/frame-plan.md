# The frame path: six defects, four of them measured

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

## 3. Two frames in flight never engage

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

**The fix.** Move the call, do not change `finishFrame`. In the game and in `bench` the wait belongs
at the top of the next iteration, before the walk. `shot` and `verify` keep it where it is, which is
what the contract already says they want.

**The catch, and it is the reason this is not a one-line change.** With the CPU genuinely a frame
ahead, the second table copy starts being read while the first is written, and every defect below
this line stops being latent. §1 is the first one that surfaced. This step goes last.

## 4. Every cell crossing throws a frame away

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

**The fix.** A placement that has to reach the queue before another placement does not need a frame
to do it. Give `placeScene` its own submit for that case, the way the doll and the map already have
one, and leave `mFrame` alone. The frame ring should count frames the caller asked for and nothing
else.

## 5. Body parts in the air, and terrain that blinks — one suspect, not yet proven

Two reports, and they are almost certainly the same defect: parts of a moving NPC stay behind, and
terrain blinks hard at a **static** camera — `-2,-9` at `-6087, -70048, 2978`, bearing 34°, climb 4°,
day 0, 12:00, Clear.

**Both were seen in `view`, and `view` is the only caller that genuinely runs two frames in flight**
(§3). So what these look like is a table copy read while it is a frame behind — the exact hazard the
second copy and `RowDebt` exist to prevent, on the only path that ever reaches it.

**What was tried and what it proved.** `bench` at that same spot is stable to 0.5 of 255 over 600
frames. Removing its `finishFrame` to make it pipeline did not change that — but the probe reads the
frame back, and `readPixels` submits and waits, so it serialises the very frame it measures. The
experiment is inconclusive rather than negative. A detector has to compare frames without waiting on
one: keep the sums on the device, or read back every fourth frame and leave the rest alone.

**Where to look, in order.**

- `SceneAcceleration` owes its rows `scene.getMoved()`; `SceneBuffers` owes `getSettled()` **and**
  `getMoved()`. The asymmetry is defensible — an acceleration row carries no motion vector — and it
  is still the first thing to prove rather than assume.
- `SceneBuffers::outgrow` doubles, so `outgrow` answers false for a table that grew into room it
  already had, and the whole-table write is skipped. `SceneAcceleration` sizes exactly and so always
  takes the whole-table branch on growth. Two tables indexed alike, growing by different rules.
- One bottom-level structure per mesh, one positions buffer per frame slot. `prepareRefit` refits
  from the current slot's copy, which is right; what has not been checked is a mesh that deforms on
  one frame and not the next while the slots alternate.

**Reproduce it first.** A test that places two instances, moves one, and asserts both table copies
carry the same rows after two placements would fail on the spot if the debt is the cause. That test
belongs in the tree whatever the answer is.

## 6. `island-crossing` differs from itself between runs — not yet root-caused

One view of sixteen, worst 4 of 255 on 0.01% of the pixels, release, upscaling off. Everything else
is byte-identical between runs of one build.

**Where to look.** It is the streaming view, so the suspects are ordering rather than arithmetic:
what order chunks arrive in, what order slots are taken back and reused, and whether a texture keeps
its index across a run. §4 changes what a crossing does, so this is worth re-measuring after §4 and
before hunting it.

## Order

1. §1, done.
2. §2, done — a deletion and a rename, and it makes every later comparison worth taking.
3. §5 — a detector that does not wait, then the table-copy test, then whatever it names.
4. §4 — take the empty frame out of the ring.
5. §6 — re-measure after §4, then hunt.
6. §3 — last, and only once §5 is closed.

**Do not give `view` the `finishFrame` its siblings have.** It would hide §5 rather than fix it, and
lose the only path in the tree that exercises what step 4 was for.
