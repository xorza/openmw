# Open issues: root causes and a plan

The route, and only what is left of it. A step that is done is **deleted** rather than marked done —
the same rule `ISSUES.md` keeps, and for the same reason: what a finished step knew now lives in the
code that does it, and a plan annotated with its own history stops being a plan.

Three of the four entries in `ISSUES.md` are not three bugs. They fall into three causes, and the
largest of them is one mistake made over and over: **this engine now has two hosts, and each derives
for itself what only one of them should decide.** The fourth is a stale comment in `instance.cpp` and
belongs to no cause. Ordered by what unblocks what, then by risk. The letters name a group rather
than count one, so a gap in them is a group that is finished.

---

## A. Two hosts derive for themselves what one of them should decide

**Retires: the fog mismatch.**

`RenderingManager` and `openmw-rtxtool` both build the same components and both hand the renderer a
frame's inputs. Nothing says how a host does that, so each divergence has had to be found in a
picture: what a lamp radiates was one, what a `Negative` record is worth was another, the loader's
process-global state a third, and the fog is what is left.

`Rtx::makeLight` is what the answer looks like — one function, two callers, no way to disagree, and
every refusal said once where both routes pass through. Fog is not there yet.

### A1 — one fog derivation, and not the rasterizer's ramp

The harness measures extinction over `Rtx::distantLandReach()` and the recorded depth
(`lightbuilder.cpp:186`): `ln2 / (reach * (1 - depth/2))`. The game measures it over the midpoint of
the ramp `MWRender::FogManager` built from `viewing distance` (`rtxrenderer.cpp:779-780`):
`ln2 / midpoint`. Both say "where half the light is gone"; they say it about different distances. At
the shipped defaults those are 32768 and 7168 units, so a screenshot and a played frame stand in
different air.

The distances differ because the *sources* do, and one of them is a rasterizer workaround:
`FogManager`'s linear ramp exists to hide a far clip plane, and this renderer has no far clip to
hide. Per the fork's own rule, it does not come across.

So the game hands `Rtx::fogExtinction` the weather's recorded depth and the reach the renderer draws
to, exactly as the harness does, and the ramp is not consulted. One derivation, one call site each
side, agreeing by construction rather than by two numbers matching.

**Changes the picture**, and it is the entry most likely to look wrong before it looks right — the
game's fog is currently far denser. Do it where `shot` can be compared against a played frame.

---

## C. The game says it with a mask, and this renderer reads none of them

**Retires: part of the actor flip.**

A cull mask is the rasterizer's vocabulary. `Renderer::showWorld` and `Renderer::toggleWorld` are the
shape that replaced two already; this is what is left.

### C2 — the actor processing range flip (measure first, then decide)

`MWMechanics::Actors` (`actors.cpp:1243-1250`) sets an actor's base node mask to zero past `actors
processing range` and back the frame after, so an actor sitting on that distance takes its carried
torch in and out of the walk a frame at a time. Unlike the rest of this group the mask is not a
rasterizer's: it says "not being simulated", which is true and upstream's business.

Do not guess. Instrument the light count against actor distance for one `bench` route and see
whether it fires at a distance where a torch still contributes. If it does, the fix is that the
mirror stops reading "not simulated" as "not in the picture" — not hysteresis.

---

## D. Frame-to-frame state with no owner

**Retires: auto-exposure.**

A value that should carry across frames and carries nothing at all. It has no place to live, and the
renderer has no statement of what a frame hands to the next.

### D1 — auto-exposure has no time constant

The histogram is measured on the frame the curve is about to map and applied to that same frame
(`vulkanrenderer.cpp:766-772`), with nothing carried between them. The picture's brightness is a pure
function of what is on screen *now*, so any one-frame excursion in the histogram is a one-frame
excursion in the whole image, and the degenerate branch (`rtxvulkan/shaders/exposure.comp:78-82`) can snap a night
exterior from an exposure of order tens to exactly `1.0` between two frames.

Measured stable at Seyda Neen at night — 0.5% across DLSS convergence, 0.2% across sub-frame camera
steps — so this is a latent sharp edge rather than something visible today. It is still the only term
in the frame with no time constant, and adaptation is a time-domain phenomenon.

Read the previous value, move toward the measured target at a rate in seconds —
`alpha = 1 - exp(-dt / tau)`, so it is frame-rate independent — and take the target outright on the
first frame and on a history reset. `sinceLastMs` already reaches the frame. Two constants, `tau` up
and `tau` down, because the eye is not symmetric.

**Changes the picture.** Wants a moving `bench` route and a look, not a still.

---

## Plan

Each step ends with the build, the filtered test binary, and — where marked — a `shot` or a `bench`
route. No step depends on a later one.

| # | Step | Retires | Risk | Picture |
|---|------|---------|------|---------|
| 1 | A1 — one fog derivation, drop the rasterizer ramp | fog mismatch | medium | **yes, large** |
| 2 | D1 — give exposure a time constant | no adaptation | medium | **yes, large** |
| 3 | C2 — measure the actor range flip, then decide | actor flip | — | — |

Steps 1 and 2 change how the game looks most and both want a moving camera to judge, so they come
last of the fixes. Step 3 is not a fix until a measurement says there is one.

Nothing here is a rewrite. The largest single change is step 1, and it is one call site each side.
