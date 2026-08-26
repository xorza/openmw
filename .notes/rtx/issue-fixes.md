# Open issues: root causes and a plan

The route, and only what is left of it. A step that is done is **deleted** rather than marked done —
the same rule `ISSUES.md` keeps, and for the same reason: what a finished step knew now lives in the
code that does it, and a plan annotated with its own history stops being a plan. What the tree, a
`--help` or a commit already answers does not belong here.

The entries that remain still fall into root causes rather than being one bug each, which is the
point of grouping them: fixing the cause retires several `ISSUES.md` entries at once and stops the
next one being written.

Ordered by what unblocks what, then by risk. Steps that change the picture are marked, because those
are the ones that want a `shot` before and after. The letters name a group rather than count one, so
a gap in them is a group that is finished.

---

## A. The mirror inherits the rasterizer's idea of "visible"

**Retires: part of the actor flip.**

The walk and the rasterizer once had four different answers to "is this node in the picture". Three
are settled — the hidden mask, the switch state and what a `LightSource` is worth. The one left is
the only one whose answer is not obviously the mirror's to change, because the mask it turns on means
something real.

### A4 — the actor processing range flip (measure first, then decide)

`MWMechanics::Actors` (`actors.cpp:1243-1250`) sets an actor's base node mask to zero past `actors
processing range` and back the frame after, so an actor sitting on that distance takes its carried
torch in and out of the walk a frame at a time. The shape is familiar — something leaves the walk and
is back a frame later — but the cause here is upstream's and legitimate: the mask says "not being
simulated".

Do not guess. Instrument the light count against actor distance for one `bench` route and see whether
it fires at all at a distance where a torch still contributes. If it does, the fix is to stop
conflating "not simulated" with "not in the picture" for the mirror — not to add hysteresis.

---

## C. Frame-lifetime GPU state with no single owner

### C1 — `mTarget` is discarded while the presenter may still be reading it

`renderFrame` discards `mTarget` from `VK_IMAGE_LAYOUT_UNDEFINED` with a `TOP_OF_PIPE` source scope
(`vulkanrenderer.cpp:634-637`) — a barrier that waits for nothing. `Presenter::present` submits its
blit **asynchronously** and waits `mPresenting[index]` only when that swapchain image comes round
again (`presenter.cpp:175`). Nothing orders the discard against the blit.

`GBuffer::begin` (`gbuffer.cpp:110-121`) has the shape to copy but not the whole answer: it discards
from `UNDEFINED` too, and orders the write-after-read by sourcing the barrier at `COMPUTE_SHADER`
rather than `TOP_OF_PIPE` — which works because its reader is in the same submit. `mTarget`'s reader
is the presenter's own submit, and no source scope reaches across one; this needs a fence, a
semaphore, or a second image.

It survives today because `placeScene` interposes a waited submit on the same queue, which is an
accident of scheduling and not a guarantee.

Two honest fixes:

- **Wait**: give `Presenter` a `waitForLastUse()` and call it before the first write. Correct, one
  line, and it can stall the frame on the previous present.
- **Double-buffer the target**: the frame never writes the image being presented. No stall, one more
  full-size image.

Prefer the second. A stall that depends on the compositor is exactly the spike the frame-time rule is
about, and one image is cheap against the structures already resident.

### C2 — `mHistoryStale` is cleared whether or not anything read it

`vulkanrenderer.cpp:767` clears it at the end of every frame, but it is consumed only when the
wavelet runs or an upscaler exists. With neither, a `resetHistory` is dropped rather than deferred.
Clear it where it is consumed, or carry it until it is. Small, and it wants a test: `resetHistory`
followed by a frame with `Denoiser::None` must still reset the frame after.

---

## D. Auto-exposure has no time constant

The histogram is measured on the frame the curve is about to map and applied to that same frame
(`vulkanrenderer.cpp:739-748`), with nothing carried between frames. The picture's brightness is a
pure function of what is on screen *now*, so any one-frame excursion in the histogram is a one-frame
excursion in the whole image, and the degenerate branch (`exposure.comp:77-83`) can snap a night
exterior from an exposure of order tens to exactly `1.0` between two frames.

Measured stable at Seyda Neen at night — 0.5% across DLSS convergence, 0.2% across sub-frame camera
steps — so this is a latent sharp edge rather than something currently visible. It is still the only
term in the frame with no time constant, and adaptation is a time-domain phenomenon.

Make the exposure buffer genuinely temporal: read the previous value, move toward the measured target
at a rate in seconds — `alpha = 1 - exp(-dt / tau)`, so it is frame-rate independent — and take the
target outright on the first frame and on a history reset. `sinceLastMs` already reaches the frame.
Two constants, `tau` up and `tau` down, because the eye is not symmetric.

**Changes the picture.** Wants a moving `bench` route and a look, not a still.

---

## E. Fog is derived twice from different distances

The harness measures extinction over `distantLandReach()` (`lightbuilder.cpp:185`); the game measures
it over the midpoint of the ramp `MWRender::FogManager` built from `viewing distance`
(`rtxrenderer.cpp:746`). At the shipped defaults those are 32768 and 7168 units, so a screenshot and a
played frame stand in different air.

The distances differ because the *sources* differ, and one of them is a rasterizer workaround:
`FogManager`'s linear ramp exists to hide a far clip plane, and this renderer has no far clip to hide.
Per the fork's own rule, it does not come across.

So: the game hands `Rtx::FogBuilder` the weather's recorded fog depth and the reach the renderer
actually draws to, exactly as the harness does, and the ramp is not consulted. One derivation, one
call site each side, and the harness and the game stand in the same air by construction rather than by
two numbers agreeing.

**Changes the picture**, and it is the entry most likely to look wrong before it looks right — the
game's fog is currently far denser. Do it where `shot` can be compared against a played frame.

---

## F. Upstream: `setObjectRoot` drops the carried light

`Animation::setObjectRoot` (`animation.cpp:1546-1556`) removes the subtree that owns
`mExtraLightSource` and never re-adds it, so an actor whose object root is rebuilt loses its carried
light **permanently** — not for a frame. This is upstream's and affects both renderers; it is just
more visible here, because here a torch is the only thing lighting the scene.

Re-attach the light in `setObjectRoot` after the new root is in. Standalone, no dependency on
anything above.

---

## Plan

Each step ends with the build, the filtered test binary, and — where marked — a `shot` or a `bench`
route. No step depends on a later one.

| # | Step | Retires | Risk | Picture |
|---|------|---------|------|---------|
| 1 | F — re-attach `mExtraLightSource` in `setObjectRoot` | carried light | low | yes |
| 2 | C1 — double-buffer `mTarget` | present race | low | no |
| 3 | C2 — clear `mHistoryStale` where it is consumed | dropped reset | none | no |
| 4 | E — one fog derivation, drop the rasterizer ramp | fog mismatch | medium | **yes, large** |
| 5 | D — give exposure a time constant | no adaptation | medium | **yes, large** |
| 6 | A4 — measure the actor range flip, then decide | actor flip | — | — |

Steps 1 to 3 are the cheap ones and none of them needs a judgement about how the game looks. Steps 4
and 5 change how it looks most, and both want a moving camera to judge — which is also why they come
after everything that would move the frame underneath them. Step 6 is not a fix at all until the
measurement says there is one.
