# Open issues: root causes and a plan

The route, and only what is left of it. A step that is done is **deleted** rather than marked done —
the same rule `ISSUES.md` keeps, and for the same reason: what a finished step knew now lives in the
code that does it, and a plan annotated with its own history stops being a plan.

Two of the three entries in `ISSUES.md` are not two bugs. They fall into two causes. The third is a
stale comment in `instance.cpp` and belongs to no cause. Ordered by what unblocks what, then by risk.
The letters name a group rather than count one, so a gap in them is a group that is finished.

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
| 1 | D1 — give exposure a time constant | no adaptation | medium | **yes, large** |
| 2 | C2 — measure the actor range flip, then decide | actor flip | — | — |

Step 1 changes how the game looks and wants a moving camera to judge. Step 2 is not a fix until a
measurement says there is one.
