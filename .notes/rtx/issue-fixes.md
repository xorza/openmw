# Open issues: root causes and a plan

The route, and only what is left of it. A step that is done is **deleted** rather than marked done —
the same rule `ISSUES.md` keeps, and for the same reason: what a finished step knew now lives in the
code that does it, and a plan annotated with its own history stops being a plan.

Two entries are left in `ISSUES.md`. One is the actor range flip below, which is not a fix until a
measurement says so; the other is a stale comment in `instance.cpp` and belongs to no cause.

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
---

## Plan

Each step ends with the build, the filtered test binary, and — where marked — a `shot` or a `bench`
route. No step depends on a later one.

| # | Step | Retires | Risk | Picture |
|---|------|---------|------|---------|
| 1 | C2 — measure the actor range flip, then decide | actor flip | — | — |

Step 1 is not a fix until a measurement says there is one.
