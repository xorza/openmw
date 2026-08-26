# Open issues: root causes and a plan

The route, and only what is left of it. A step that is done is **deleted** rather than marked done —
the same rule `ISSUES.md` keeps, and for the same reason: what a finished step knew now lives in the
code that does it, and a plan annotated with its own history stops being a plan.

Nine entries in `ISSUES.md` are not nine bugs. They fall into five causes, and the largest of them is
one mistake made three times: **this engine now has two hosts, and only one of them was ever
configured.** Ordered by what unblocks what, then by risk. The letters name a group rather than count
one, so a gap in them is a group that is finished.

---

## A. Two hosts stand up one engine, and only one was configured

**Retires: the fog mismatch, negative lights, and the harness's hidden node mask.**

`RenderingManager` and `openmw-rtxtool` both build the same components and both hand the renderer a
frame's inputs. Nothing says how a host does that, so each divergence has had to be found in a
picture: the lights were one (`lightColour`, since fixed), and these are the rest.

Two different shapes of divergence, and they want different answers.

**Process-global configuration a second host does not know to set.** `NifOsg::Loader` keeps three
statics — `setHiddenNodeMask`, `setIntersectionDisabledNodeMask`, `setSoftEffectEnabled` — and
`renderingmanager.cpp:407-409` is the only place any of them is written. `sHiddenNodeMask` defaults
to **zero** (`nifloader.cpp:315`), and zero is not a harmless default: a node the content hides gets
a node mask of nothing, so no visitor reaches it, so the `NifOsg::VisController` that would animate
it visible never runs. A node hidden at load stays hidden for the life of a harness run.

`Terrain::ObjectPaging` reads the same static back (`objectpaging.cpp:442`) to decide what to copy
into distant land, so under the harness `copyMask` is `~0` — it copies everything, and only avoids
copying a town's collision meshes into its hills because those nodes carry no bits at all. Two
wrongs, agreeing.

The fix is not to teach the harness the three calls. It is that "nobody set it" must stop being a
state the engine runs in: one initialiser both hosts make, and a hidden mask whose default cannot be
a value that silently disables a feature.

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

### A3 — one preparation, made by both hosts

Give the engine a single call that says "prepare this for the ray tracer", made by
`RenderingManager` and by the harness, carrying the loader's three statics. Then make the hidden node
mask impossible to leave unset — a default of zero is a working-looking configuration in which
NIF visibility animation does not exist.

**Its own negative control**: with the harness setting a real mask, a hidden node stops being skipped
for lack of bits and starts being skipped for the mask the walk excludes — the same answer by the
route the game takes. The census must not move.

---

## B. The mirror's answer to "which children are in the picture"

`MirrorTraversal::descend` honours `osg::Switch`, deliberately does not honour `osg::LOD` — a ray is
owed the finest child, not the one a distance test picked for an eye — and has no answer at all for
`osg::Sequence`, which is the third node in the tree that selects among its children.

`osg::Sequence` visits every child under `TRAVERSE_ALL_CHILDREN` **and** advances its own clock only
under `TRAVERSE_ACTIVE_CHILDREN`. `NifOsg` builds one for every `NiFltAnimationNode`
(`nifloader.cpp:987`), which is Morrowind's flipbook animation, so every frame of a flipbook is
traced at once, in the same place, and none of them ever advances.

The fix is one more branch in `descend`, and a decision written down once for all three: a switch is
honoured, a sequence is honoured *and stepped*, an LOD is not. Stepping is the part that is not
obvious — like `osgParticle`, the clock lives in a traversal this renderer does not run, so the walk
has to be what runs it.

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

**Retires: the dropped history reset, and auto-exposure.**

Two values that should carry across frames. One is cleared by whoever ends the frame rather than by
whoever reads it; the other carries nothing at all. Neither has a place to live, and the renderer has
no statement of what a frame hands to the next.

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

### D2 — `mHistoryStale` is cleared whether or not anything read it

`vulkanrenderer.cpp:796` clears it at the end of every frame, but it is consumed only where the
wavelet runs or an upscaler exists. With neither, a `resetHistory` is dropped rather than deferred.
Clear it where it is consumed, or carry it until it is — and note that D1 adds a third consumer, so
this is the cheaper half of the same statement.

Small, and it wants a test: `resetHistory` followed by a frame with `Denoiser::None` must still reset
the frame after.

---

## E. A surface describing a design this fork does not have

`Rtx::Renderer::shareFrame` documents the OpenGL interop path — "the SDL window stays OpenGL's, and
Vulkan renders offscreen into an image OpenGL imports and draws under the GUI". That is the design
this fork explicitly abandoned: with the ray tracer on, no GL context is created at all. It has no
caller outside a test stub, and the frame is now one of a pair swapped every present, so a single
exported allocation could not answer for it even if something did import one.

Delete it — the virtual, both implementations and the `SharedFrame` type. The Metal backend is the
one question to settle first, and it is the same answer: a backend that owns its own surface presents
through it.

---

## Plan

Each step ends with the build, the filtered test binary, and — where marked — a `shot` or a `bench`
route. No step depends on a later one.

| # | Step | Retires | Risk | Picture |
|---|------|---------|------|---------|
| 1 | E — delete `shareFrame` and `SharedFrame` | dead interop | none | no |
| 2 | D2 — clear `mHistoryStale` where it is consumed | dropped reset | none | no |
| 3 | A3 — one engine preparation both hosts make | harness hidden mask | low | harness only |
| 4 | B — `descend` honours and steps `osg::Sequence` | flipbooks | low | yes, animated textures |
| 5 | A2 — `makeLight` rejects what it cannot express | negative lights | none | rare, and wrong today |
| 6 | C1 — `tws` through the renderer seam | half a toggle | low | debug only |
| 7 | A1 — one fog derivation, drop the rasterizer ramp | fog mismatch | medium | **yes, large** |
| 8 | D1 — give exposure a time constant | no adaptation | medium | **yes, large** |
| 9 | C2 — measure the actor range flip, then decide | actor flip | — | — |

Steps 1 and 2 are free. Steps 3 to 6 are each one decision moved to where it can only be made once.
Steps 7 and 8 change how the game looks most and both want a moving camera to judge, so they come
after everything that would move the frame underneath them. Step 9 is not a fix until a measurement
says there is one.

Nothing here is a rewrite. The largest single change is step 7, and it is one call site each side.
