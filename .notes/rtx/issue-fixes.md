# Open issues: root causes and a plan

The route, and only what is left of it. A step that is done is **deleted** rather than marked done —
the same rule `ISSUES.md` keeps, and for the same reason: what a finished step knew now lives in the
code that does it, and a plan annotated with its own history stops being a plan.

Five of the six entries in `ISSUES.md` are not five bugs. They fall into three causes, and the
largest of them is one mistake made over and over: **this engine now has two hosts, and each derives
for itself what only one of them should decide.** The sixth is a stale comment in `instance.cpp` and
belongs to no cause. Ordered by what unblocks what, then by risk. The letters name a group rather
than count one, so a gap in them is a group that is finished.

---

## A. Two hosts derive for themselves what one of them should decide

**Retires: the fog mismatch, and negative lights.**

`RenderingManager` and `openmw-rtxtool` both build the same components and both hand the renderer a
frame's inputs. Nothing says how a host does that, so each divergence has had to be found in a
picture: the lights were one (`lightColour`, since fixed), the loader's process-global state was
another, and these are the rest.

**Derivations each host makes for itself.** `Rtx::makeLight` is what this should look like — one
function, two callers, no way to disagree. Fog is not there yet, and neither is what a `LIGH` record
is worth once the graph has built it.

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

### A2 — `makeLight` owns what a ray tracer cannot express

`makeLight(const ESM::Light&)` drops a `Negative` record, because a light that *subtracts*
illumination is a trick available to a renderer accumulating into a framebuffer and meaningless to
one that traces a ray to an emitter. The graph path never sees that flag:
`SceneUtil::createLightSource` (`lightutil.cpp:130`) negates the diffuse instead, so the walk mirrors
a light of negative intensity exactly where the harness places none.

The flag test is in the wrong place. `makeLight(colour, radius, position)` is where both paths meet
and already owns "this is not a light" for a radius; a colour with a negative channel is the same
statement about the same thing. Moving it there makes the two agree by construction and leaves the
record's flag as what it is — a record property, still worth reading early.
---
---

## C. The game says it with a mask, and this renderer reads none of them

**Retires: `tws`, and part of the actor flip.**

A cull mask is the rasterizer's vocabulary. `Renderer::showWorld` is the shape that replaced one
already; these are what is left.

### C1 — `tws` hides half the world

`RenderingManager::toggleRenderMode(Render_Scene)` (`renderingmanager.cpp:746-756`) hides the world
by flipping `sToggleWorldMask` in the master camera's cull mask. The RT path culls nothing and reads
that mask nowhere, so the console command does nothing to the traced picture — except the water,
which goes through `Water::showWorld` and works. Half a toggle is worse than none.

Say it on the seam, as `showWorld` already does. It is a second, independent reason not to show the
world — a loading screen during `tws`-off must not turn it back on — so the renderer holds two
answers and shows the world when both say so.

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
| 1 | A2 — `makeLight` rejects what it cannot express | negative lights | none | rare, and wrong today |
| 2 | C1 — `tws` through the renderer seam | half a toggle | low | debug only |
| 3 | A1 — one fog derivation, drop the rasterizer ramp | fog mismatch | medium | **yes, large** |
| 4 | D1 — give exposure a time constant | no adaptation | medium | **yes, large** |
| 5 | C2 — measure the actor range flip, then decide | actor flip | — | — |

Steps 1 and 2 are each one decision moved to where it can only be made once. Steps 3 and 4 change how
the game looks most and both want a moving camera to judge, so they come after everything that would
move the frame underneath them. Step 5 is not a fix until a measurement says there is one.

Nothing here is a rewrite. The largest single change is step 3, and it is one call site each side.
