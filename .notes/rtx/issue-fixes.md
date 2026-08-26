# Open issues: root causes and a plan

The route, and only what is left of it. A step that is done is **deleted** rather than marked done —
the same rule `ISSUES.md` keeps, and for the same reason: what a finished step knew now lives in the
code that does it, and a plan annotated with its own history stops being a plan.

Six of the seven entries in `ISSUES.md` are not six bugs. They fall into four causes, and the largest
of them is one mistake made over and over: **this engine now has two hosts, and each derives for
itself what only one of them should decide.** The seventh is a stale comment in `instance.cpp` and
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

## F. The harness derives for itself what the game derives once

**Retires: the corridor of ground with no statics on it.**

Group A is two hosts configuring one engine. This is the same mistake one level up: two hosts
deciding what a cell grid *is*, and only one of them from a rule.
`MWWorld::Scene::changeCellGrid` takes a centre and a half size and answers both questions from them
in the one function — which cells are loaded (`iterateOverCellsAround`) and what rectangle the
terrain is handed (`gridCenterToBounds`). The harness answers the first the same way, by a copy, and
the second by a rule that exists nowhere else.

`RtxTool::World::buildTerrain` is called once per arriving cell and unions each into `mActiveGrid`
(`world.cpp:270`), so what reaches `Terrain::World::setActiveGrid` is every cell the run has ever
loaded. `dropCellsOutside` meanwhile keeps the 3×3 square around the centre. A cell between the two
is in neither picture: its own group is gone, and `ObjectPaging::getChunk` returns nothing for a
chunk the quad tree marked active-grid — which is what the harness asked for with
`pageActiveGrid=false`, so that a loaded cell is not stood twice. The ground survives because
`Terrain::ChunkManager` makes no such refusal, so a camera that moves leaves a corridor of ground
with the trees taken off it.

**Reusing `MWWorld::Scene` is not the answer, and saying so once should stop the question coming
back.** It needs `MWWorld::World`, `WorldModel`, `CellStore`, `MWPhysics`, `DetourNavigator`,
`MWBase::Environment` and a `Loading::Listener` — which is the game, and the harness exists to run
without it. The reuse available is downward, not sideways: what both hosts *derive* belongs in
`components/`, and the orchestration stays two. Group A moved three such derivations already
(`makeLight`, `makeSkylight`, the loader's configuration), and each one closed a divergence that had
to be found in a picture first.

### F1 — one cell grid, from a centre and a half size

Give `components/` a value that owns a centre and a half size and answers everything either host asks
of a cell grid: the `osg::Vec4i` `Terrain::World::setActiveGrid` takes, the square of cells in the
order the game fills it — nearest first, ties by distance to the origin — and whether a cell is in
it. `MWWorld::Scene` drops `gridCenterToBounds`, `iterateOverCellsAround` and `sortCellsToLoad` for
it. The harness drops `squareAround` and stops accumulating: `readRegion`, `dropCellsOutside` and the
call to `setActiveGrid` all read the one grid, so the loaded set and the rectangle cannot say
different things.

`cellscene.cpp:41` argues that twenty lines of arithmetic are a cheaper copy than lifting three
upstream files. This bug is what the copy cost, and it was not the arithmetic that drifted — it was
the fourth caller nobody thought of.

**Its own negative control**: cross a route and come back, and the statics have to come back with the
camera. `RtxCrossingTest.walkingAcrossManyCellsHoldsAGridRatherThanEverythingVisited` already stands
next door and asserts the loaded set is bounded, which is the half that was right.

### F2 — one table of node-mask bits

`MWRender::VisMask` lives in `apps/openmw/mwrender/vismask.hpp`, which the harness does not link, so
it writes the bits out by hand: `sWaterMask` is `1u << 6` (`waterplane.hpp:18`), `sLightMask` is
`1u << 19` (`cellscene.cpp:211`), and the hidden node mask is `1u << 0` (`world.cpp`). Each carries a
comment naming the constant it copies, which is the tell.

Move the table to `components/`, rewrite its thirty-three includes, and let the harness name what it
means. `sToggleWorldMask` goes with it, which is what C1 wants at the seam. OpenCS keeps its own
enum: that is a different application's vocabulary rather than a copy of this one.

Nothing about the picture changes. It is a move, and its value is that the next copied constant
cannot be written.

---

## Plan

Each step ends with the build, the filtered test binary, and — where marked — a `shot` or a `bench`
route. No step depends on a later one.

| # | Step | Retires | Risk | Picture |
|---|------|---------|------|---------|
| 1 | F1 — one cell grid, from a centre and a half size | vanishing statics | low | **yes, harness** |
| 2 | F2 — one table of node-mask bits | three copied constants | none | no |
| 3 | A2 — `makeLight` rejects what it cannot express | negative lights | none | rare, and wrong today |
| 4 | C1 — `tws` through the renderer seam | half a toggle | low | debug only |
| 5 | A1 — one fog derivation, drop the rasterizer ramp | fog mismatch | medium | **yes, large** |
| 6 | D1 — give exposure a time constant | no adaptation | medium | **yes, large** |
| 7 | C2 — measure the actor range flip, then decide | actor flip | — | — |

Step 1 is the only outright breakage left and comes first. Steps 2 to 4 are each one decision moved
to where it can only be made once, and step 2 is what step 4 wants to say `tws` at the seam. Steps 5
and 6 change how the game looks most and both want a moving camera to judge, so they come after
everything that would move the frame underneath them. Step 7 is not a fix until a measurement says
there is one.

Nothing here is a rewrite. The widest change is step 2, and every line of it is a move.
