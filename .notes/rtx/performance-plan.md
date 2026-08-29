# RTX performance plan

The route from `performance.md` (2026-08-29) to the 1920×1080 → 3840×2160 at 60 fps target, as
ordered steps. Each step names what the code does today, what changes, and the number that says
it worked. The numbers quoted as "today" are the ship at 1080p quality unless said otherwise.

**Done before this plan** (`4e416fdfe7`): `shade()` writes rows and runs, the sea is skipped where a
frame has no water, the composite bake is on its own thread and an arrival rides the placement's
submit. Ship 1 % low 26 → 48 fps at the same median; guild 78 → 90 fps.

**Steps 1–3 done.** The placement rewrites the rows the scene names (`getMoved` and `getSettled`)
and keeps the top level; deforming meshes are built with `ALLOW_UPDATE` and refitted with
`MODE_UPDATE`, and an unchanged pose names nothing; the fade is resolved as the shading chain is
built and the walk's casts are gated on the library. Same-state A/B, release, 1080p quality:
ship `place` 2.05 → 1.05 ms, frame 13.3–14.4 → 11.8 median, p99 18–20 → 16, 1 % low 50–55 → 61–64
fps, `refit` GPU zone 0.40 → 0.13 ms; guild frame 10.5 → 10.0, `place` 1.16 → 0.74, 95 → 100 fps.

**Step 4 done.** Two frames in flight on a ring of two slots: a frame is a placement submit without
a fence and a trace submit with one, the fence is waited where the frame after next wants the slot,
and `finishFrame` hands the result back a frame late. Every table a frame writes has a copy per slot
with a `RowDebt` saying what each copy still owes; what a frame in flight may read goes to its
`Graveyard` and is destroyed at its fence. An arrival waits (`extendScene`), and a picture inside
the interface waits for everything. The bench rows say `wait` where they said `trace`: what the CPU
stood still for the device. Same-state A/B against steps 1–3, release, 1080p quality: ship frame
11.8 → 11.0 median, p99 16 → 15.8, 1 % low 61–64 → 63 fps; guild 10.0 → 9.6, 100 → 104 fps;
streaming route 81.5 fps, 1 % low 3.9, worst crossing 338 ms; `place` 1.05 → 0.49 (ship) and
0.74 → 0.26 (guild), which is the fenced build coming off it. **The frame is the device's now:**
the CPU waits 6.5 of the ship's 11 ms and 8.1 of the guild's 9.6, so the ~8 ms this step was
sized at was the sum's other half — the device alone is 11 ms at this setting and no pipeline goes
under it. From here the number moves with the trace, which is steps 5–11. `verify` against a
build of the commit before: fifteen of sixteen views byte-identical, and `island-crossing` differs
from itself run to run in either build.

**Steps 5 and 6 done.** `visibility.comp` takes four specialization constants beside `COUNT_HITS` —
`HAS_SUN`, `HAS_MOONS`, `HAS_SEA`, `FOG_UNIFORM` (`lib/variants.glsl`) — each standing in front of
the runtime test it replaces rather than in place of it, so a variant removes dead code and never an
answer. `VisibilityVariant` resolves them from the frame's own constants and from whether the scene
holds a water surface at all, and `VisibilityPass` keeps a pipeline per tuple: the exterior day, the
exterior night and the interior are compiled at construction, the rest on the frame that first asks.
The interior module is 188 KiB of binary against 1377 KiB unspecialized, and the night's register
count is the only one that moves (96 → 128). `verify` against an unspecialized build: sixteen of
sixteen byte-identical, `island-crossing` excepted as always.

Step 6 needed its premise corrected. **Morrowind lights every interior with a directional light**
(`Sky::roomSun`), so `!HAS_SUN` is not what a room is, and the guard the plan named would have fired
in no cell in the game — every one of the six interior `verify` views resolves to a sun. So the
closed form is guarded on `FOG_UNIFORM` alone and carries the sun and the moons: the transmittance is
`exp` of a closed-form column, the ambient is `colour * (1 - T)` exactly, every lamp is integrated
along the ray in one walk of the light grid — `falloffAlong` is the windowed inverse square
integrated in closed form, checked against quadrature to eight digits — and what is left is the
eight stretches the shadow rays were always the whole cost of, each weighed by `T(from) - T(to)`.
The twenty-four steps and their twenty-four walks of the lamp grid are gone.

Same-state A/B, release, `shot --repeat=200`, against a build with the tuple forced to the general
kernel: guild trace 5.16 → 4.47 (step 5) → 2.76 ms, each the median of three warm runs that agree to
a hundredth. **The ship is not quotable on this box today** — four back-to-back runs of the same
binary span 3.36 to 4.48 ms with the card idling at 870 MHz between them, and what either step is
worth outdoors is inside that. Step 5 is the only one that touches an exterior at all: step 6 runs
where the coverage field is even, and outdoors it is not.

`verify`: every exterior byte-identical, and the seven interiors that carry fog differ because the
march's lamp noise is gone. Against a converged reference (`--accumulate`, filter and upscaler off)
the two agree — at 64 samples the march is still 8 of 255 away on 12% of the guild, at 256 samples
on 0.35%, and the signed mean difference is 0.09 of 255 at both.

**The rule this plan lives under.** *Feature-complete first, then fast.* Steps 1–3 and 5 are
structural and change no picture, so they can land whenever the frame they fix is in front of you.
Everything else waits for the renderer to draw everything the game has, and is measured again then.

**The budget.** At the target the GPU alone is 14.5 ms (ship) and 18.3 ms (guild) against 16.7, and
the CPU adds 3–6 ms in series. So two things have to happen, in this order: the CPU leaves the
critical path (steps 1–4), and the trace loses 3–6 ms at 1080p (steps 5–11). The crossings and the
walk (12–15) are about the 1 % low on a route, not about the median.

---

## 0. A measurement per step, first

Every step below ends in one of these, run from `build-release/` with `--validation=false`:

- `bench --views seyda-neen-ship --seconds 10 --warmup 1 --window=false` and the same for
  `balmora-mages-guild`: `frame`, `place`, `trace` medians and p99, the 1 % low.
- `shot --view <place> --repeat=200`: the GPU `trace` zone for a shader A/B. Two hundred, not
  thirty — the clock has not settled at thirty.
- `bench --suite=streaming --seconds 10`: `crossBuildMs`, the worst crossing, the 1 % low.
- `verify` before and after any step that must not change the picture.

Add one number the bench does not print yet: the walk's wall time per frame (`frame − trace −
place` today, which also hides the harness's own step). Print it as its own column in
`apps/rtxtool/bench.cpp`. Half of the CPU side is the walk and nothing measures it directly.

---

## 7. Outdoor fog, the cheaper halves

Only while the froxel volume (step 11) is not yet built.

- Weigh lamps at the 8 probes rather than at the 24 steps: most of the loop is `weighLamps`.
- `FOG_STEPS 12`, `FOG_SHADOW_RAYS 4` behind a constant, A/B'd at 200 repeats: −0.7 ms ship,
  −1.2 ms at dawn, for noise Ray Reconstruction is already removing.

**Number.** Ship trace −0.5 to −0.7 ms; check dawn (`--hour 6.5`) as well as noon.

## 8. Trace the bounce at half resolution

**Today.** `bounceLight` per pixel (`visibility.comp:56`), with its own sun, lamp and sky rays at
the bounce hit: 30–40 % of the trace, 1.0–1.4 ms.

**Change.** One bounce per 2×2 quad, shared through the albedo demodulation Ray Reconstruction
already does — or checkerboarded across frames and reprojected. A half-resolution indirect channel
in `GBuffer`, filled up by the composite.

**Number.** Ship −1.0, guild −1.1, dawn −1.4 ms. Judge with RR on: it is built for this input.

## 9. Make the micromaps cover the canopy

**Today.** 93.8 % of cutout candidates on the ship "still ask" — nearly every one runs the any-hit
shader with a texture fetch: 0.5 ms at noon, 1.7 ms at dawn. `sSubdivisionCeiling = 5`
(`micromap.hpp:72`).

**Change.** First find out why: tally per texture which level the classification ran at against
the level the shadow ray's cone reads, and how much of each map is `unknown` because the alpha sits
in the threshold band. Then either classify at the level the shadow ray reads, accept a two-state
map on the alpha's own coarse level, or raise the ceiling for the textures that are all canopy.

**Number.** `shot` micromap tally: opaque + transparent from 6 % to over 50 %; trace −0.3 noon,
−1.5 dawn.

## 10. Rays the bounce hit does not need

- One lamp shadow ray per pixel: keep the reservoir at the primary hit and let the bounce hit take
  its lamp unshadowed. −0.4 to −0.7 ms, most in interiors.
- `skyReaching` at the bounce hit every other frame, or an occlusion proxy: −0.3 to −0.6 ms
  outdoors, nothing indoors.

Both are one-bounce-deep biases. Judge them against a reference, last among the GPU items.

## 11. A froxel fog volume

**Today.** The per-pixel march is 44–66 % of the trace everywhere, and it integrates the same field,
the same lamps and the same eight sun probes for every pixel of every frame.

**Change.** A frustum-aligned grid — 240×135×64 at 1080p — integrated once per froxel per frame in
a compute pass before the trace, with temporal reprojection against the last frame's volume, and
one trilinear fetch per pixel in the trace. What every shipping volumetric does; at the fog's
900-unit grain a froxel is far finer than a bank. Replaces steps 6–7 outdoors; the interior closed
form stays, because it is exact and cheaper still.

**Number.** Ship −1.5, guild −3.2, dawn −3.2 ms — the largest GPU lever there is. After 4, 5, 6, 8
and 11 the 4K-performance ship should sit near 11 ms with room for the moons at dawn.

## 12. Crossings: fold once, and without a sort

**Today.** `SheetFold::fold` (`sheetfold.cpp:52`) canonicalises and sorts every triangle of every
arriving mesh — 21 % of a crossing's CPU, ~125 ms per crossing — and runs once per merged paging
chunk, so a source drawable merged into many chunks is folded many times.

**Change.** A hash set of canonical corner triples, O(n); fold once per source drawable at load,
keyed in the extractor's identity map, so a merged chunk reuses its sources' answers.

**Number.** `bench --suite=streaming`: `crossBuildMs` 70 → ~30 ms per crossing.

## 13. Crossings: estimate a texture once

**Today.** `SceneTextures` estimates a `ShadingMap` for every texture that arrives, every time it
arrives; a ring that leaves and comes back estimates its textures again. The composite queue keeps
a per-file cache for the baker; the uploader has none.

**Change.** A per-path cache on the uploader for the life of the process — the same shape as the
queue's, keyed by `VFS::Path::Normalized`. A cell's worth is a few hundred kilobytes.

**Number.** `ShadingMap::ShadingMap` leaves the crossing profile.

## 14. Crossings: build arrivals beside the frame

**Today.** Arriving structures and textures are built in the placement's submit, which the frame
waits on. The worst crossing is 344 ms and the 1 % low on the island route is 4 fps.

**Change.** A second queue (async compute, or transfer for the uploads) in `Device`; arrivals are
built there and swapped in when their fence signals, with the old top level traced until then.
Needs step 4's fence-per-frame and deferred-destruction machinery, so it comes after it.

**Number.** The worst crossing becomes tens of milliseconds of hitch; the route's 1 % low 4 → 30+.

## 15. The incremental walk

**Today.** The graph is walked whole every frame — 1.7 ms at 4 900 instances in the game, 2.5–3 at
8 500 in the harness — to discover that 99 % of it did not move. `distant land cells` is a CPU dial
because of it: 2 → 8 cells costs 3 ms of walk and 0.3 of trace.

**Change.** Cache the extraction of a subtree that cannot change — no update callbacks below it
(`getNumChildrenRequiringUpdateTraversal() == 0`), no skeleton, no particle system, an unchanged
parent transform — as a list of (slot, local transform), and re-place from the cache without
visiting. Everything under an actor keeps the full walk.

**Number.** The walk column 1.5 → under 0.5 ms in the game.

**Order.** Last, and possibly never: after step 4 the walk is hidden behind a GPU frame that is
longer than it, and at the 4K target the GPU is 14 ms against the CPU's 3–6. Do it when the CPU
is on the critical path again — a higher `distant land cells`, or a faster trace than this plan
reaches.

---

## The order, in one place

| step | what | gain (ms, 1080p) | picture | after |
|---|---|---:|---|---|
| 0 | a walk column in `bench` | — | — | — |
| 7 | outdoor fog, cheaper halves | −0.7 GPU | noisier | 6 |
| 8 | half-resolution bounce | −1.0 to −1.4 GPU | RR-judged | 4 |
| 9 | micromaps cover the canopy | −0.3 to −1.5 GPU | none | 4 |
| 10 | fewer rays at the bounce hit | −0.7 to −1.3 GPU | biased | 8 |
| 11 | froxel fog volume | −1.5 to −3.2 GPU | reprojected | 6 |
| 12 | fold once, without a sort | −40 ms/crossing | none | 0 |
| 13 | estimate a texture once | crossing CPU | none | 0 |
| 14 | arrivals on a second queue | worst crossing ÷ 10 | none | 4 |
| 15 | the incremental walk | −1 to −2.5 CPU | none | 4, if ever |

Steps 1 to 6 are done and their numbers are at the top of this file. After 8 and 11 the
4K-performance frame fits in 16.7 ms with room for the moons at dawn.
