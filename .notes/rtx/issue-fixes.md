# Open issues: root causes and a plan

Twelve entries in `ISSUES.md` are not twelve bugs. They fall into six root causes, and four of them
are one mistake made repeatedly: **the RT path inherited a rasterizer's answer to a question a ray
tracer asks differently.** Fixing the root cause retires several entries at once and stops the next
one being written.

Ordered by what unblocks what, then by risk. Steps that change the picture are marked, because those
are the ones that want a `shot` before and after.

---

## A. The mirror inherits the rasterizer's idea of "visible"

**Retires: hidden nodes, switch branches, `getEmpty`, glow lights, and part of the actor flip.**

The walk decides what to mirror with one traversal mask and `TRAVERSE_ALL_CHILDREN`
(`components/rtx/sceneextractor.cpp:280`). The rasterizer decides with a cull mask **and** the switch
state **and** `getEmpty` **and** per-frame node-mask writes. Two different definitions of "in the
picture", and the mirror has the looser one, so it traces things nothing draws and drops lights
everything draws.

There is no single place today that answers "is this node in the world this frame" or "what is this
`LightSource` worth". Both should exist.

### A1 — honour the hidden node mask

`sWorldTraversal` (`apps/openmw/mwrender/rtx/rtxrenderer.cpp:83`) excludes `Mask_Sky | Mask_Sun |
Mask_SimpleWater`. `RenderingManager` installs `Mask_UpdateVisitor` as `NifOsg::Loader`'s hidden node
mask (`renderingmanager.cpp:405-407`) and the cull mask excludes it; the walk does not, so every NIF
node a model marks hidden is mirrored and traced. `CharacterPreview` already gets this right
(`characterpreview.cpp:68`).

Add `Mask_UpdateVisitor` to the exclusion. One line.

**Done.** What it actually buys is narrower and sharper than "hidden nodes stop being traced":
`nifloader.cpp:872-887` already skips creating meshes for a hidden node *unless* it carries a
`NiVisController`, so most hidden nodes had no geometry to trace either way. The ones that do are
exactly the nodes whose visibility is animated — and `NifOsg::VisController` animates it by swapping
the node between the hidden mask and every bit (`components/nifosg/controller.cpp:393`). So the walk
ignored NIF visibility animation entirely; now it follows it.

Measured as a negative control: the census office builds 207 meshes into 817 instances with 8 lights
before and after, so nothing was wrongly excluded. `apps/rtxtool/picture.cpp:37` carried narration
that this change made stale — the harness installs no hidden mask, so a hidden node there has no bits
at all and is skipped by every visitor already; the two walks still agree, by two different routes.
That in turn is a harness quirk of its own, and it is in `ISSUES.md`.

### A2 — honour `osg::Switch`

`osg::Switch::traverse` visits **every** child under `TRAVERSE_ALL_CHILDREN`, so a branch
`DayNightCallback` (`animation.cpp:107-131`) switched off is mirrored: a night-only lamp is traced at
noon and the day mesh is traced at midnight, both at once. This is geometry, not only light.

Give `MirrorTraversal` an `apply(osg::Switch&)` that visits only the enabled children. Prefer that
over changing the traversal mode, because the walk *wants* all children everywhere else — an
`osg::LOD` should give the ray tracer its finest child, not the one a distance test picked for an eye
that does not constrain a ray.

### A3 — what a `LightSource` is worth to a ray tracer

Two defects, one cause: `SceneExtractor::addLight` (`sceneextractor.cpp:726-750`) reads a
fixed-function light the way a fixed-function pipeline did.

- It reads only `getDiffuse()`. `Animation::setLightEffect` (`animation.cpp:1920-1923`) writes a zero
  diffuse and a bright **ambient** for a glow light, so every Light spell and every enchanted item
  contributes exactly nothing.
- It drops the light when `getEmpty()`. That flag means "the model this light hangs on has no
  geometry" (`CheckEmptyLightVisitor`, `lightutil.cpp:15-38`) — a rasterizer's reason to skip a light,
  and not a statement that the light is off. The harness already had to override it for props
  (`apps/rtxtool/cellscene.cpp:289`), which is the tell.

Put one function beside `makeLight` in `lightbuilder` that takes a `SceneUtil::Light` and answers
what it radiates — diffuse plus ambient, since the content uses both — and make the walk consult it
instead of reaching into the light itself. Drop the `getEmpty` test; delete the harness override in
the same step, and its disappearance is part of the proof.

**Changes the picture.** `shot --view=seyda-neen-customs` and `--view=balmora-mages-guild` before and
after; a Light spell has no harness view, so a game frame is the only check for that half.

### A4 — the actor processing range flip (measure first, then decide)

`MWMechanics::Actors` (`actors.cpp:1243-1250`) sets an actor's base node mask to zero past `actors
processing range` and back the frame after, so an actor sitting on that distance takes its carried
torch in and out of the walk a frame at a time. This is the same *shape* as the sweep bug just fixed,
but the cause is upstream's and legitimate: the mask says "not being simulated".

Do not guess. Instrument the light count against actor distance for one `bench` route and see whether
it fires at all at a distance where a torch still contributes. If it does, the fix is to stop
conflating "not simulated" with "not in the picture" for the mirror — not to add hysteresis.

---

## B. Three owners drive one update traversal

**Retires: the double traversal, and the `enableScene` mask.**

`RtxRenderer` owns the update visitor, `Stage` parents the scene root under the camera, and
`WindowManager` reaches in and mutates the visitor's mask. Any two of them can disagree.

### B1 — stop walking the scene twice a frame

`RtxRenderer::updateTraversal` (`rtxrenderer.cpp:322-333`) accepts the visitor on the scene root, then
accepts it on the master camera to reach `MWRender::Camera`'s callback. But `Stage::setSceneRoot`
(`stage.cpp:82`) has parented the scene root **under that camera**, and
`UpdateRenderCameraCallback::operator()` traverses its children before doing its own work
(`camera.cpp:34-40`). So the second accept descends the whole world again at the same traversal
number: every animation controller, every `LightController` and `LightManager::update` runs twice per
frame.

**The obvious fix is a trap.** Dropping the scene-root accept and keeping only the camera's looks
like one traversal that does both jobs — and it is, but the node path then starts at an `ABSOLUTE_RF`
camera, so `osg::computeLocalToWorld(nv->getNodePath())` in `CollectLightCallback`
(`lightmanager.cpp:114`) folds the view matrix into every light's world transform. Harmless only
because the RT path reads its own transforms rather than `Light::getPosition()` — which is a
landmine, not a fix.

Fix the parenting instead: the camera has children only because `osgViewer` needs them, and the RT
path renders through no camera at all. Make `Stage::setSceneRoot` parent the root for the GL renderer
and not for this one, and `mCamera->accept` then runs the callback over a camera with nothing under
it. Verify by asserting the camera has no children on the RT path, and by counting controller ticks
before and after.

**Worth measuring**: this is half of every update traversal in the game, and it is the one place in
this list where the number is likely to be large.

### B2 — `enableScene` mutates a visitor it does not own

`WindowManager::enableScene` (`windowmanagerimp.cpp:622-637`) blanks the traversal mask of the update
visitor the RT renderer owns, and sets a cull mask. On the RT path there is no cull, so the cull half
is inert and the update half is the whole effect: while it holds, the update reaches no scene node
while the mirror keeps its own `sWorldTraversal` and walks anyway — reading state nothing is
refreshing.

`getUpdateVisitor` has exactly one consumer outside the renderers, and it is this. Replace it with a
statement of intent on the renderer — "the world is not being shown" — which the GL path answers with
its masks and the RT path answers by not walking and not tracing. Then drop `Stage::getUpdateVisitor`
from the interface, so the shared handle cannot be reached at all.

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
| 1 | **done** — A1, exclude `Mask_UpdateVisitor` from `sWorldTraversal` | hidden nodes | none | visibility animation only |
| 2 | A2 — `MirrorTraversal::apply(osg::Switch&)` | switch branches | low | yes |
| 3 | A3 — one function for what a light radiates; drop `getEmpty`; delete the harness override | glow lights, `getEmpty` | low | yes |
| 4 | B1 — stop parenting the scene root under the camera | double traversal | medium | no |
| 5 | B2 — replace `enableScene`'s mask reach-in; drop `Stage::getUpdateVisitor` | shared visitor | low | no |
| 6 | F — re-attach `mExtraLightSource` in `setObjectRoot` | carried light | low | yes |
| 7 | C1 — double-buffer `mTarget` | present race | low | no |
| 8 | C2 — clear `mHistoryStale` where it is consumed | dropped reset | none | no |
| 9 | E — one fog derivation, drop the rasterizer ramp | fog mismatch | medium | **yes, large** |
| 10 | D — give exposure a time constant | no adaptation | medium | **yes, large** |
| 11 | A4 — measure the actor range flip, then decide | actor flip | — | — |

Steps 1–3 first: they are the cheapest, they are all the same mistake, and each one makes the frame
more correct rather than merely different. Step 4 is the only one with a measurable performance
answer and no visual one, so it is worth doing before anything is measured again. Steps 9 and 10
change how the game looks most, and both want a moving camera to judge — leave them until the ones
above have stopped moving the frame underneath them.

Nothing here is a rewrite. The largest single change is step 3, and it is one function and two
deletions.
