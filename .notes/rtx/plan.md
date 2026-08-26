# Implementation plan

An experimental ray-traced renderer inside this OpenMW fork. The posture and the priority order live
in `CLAUDE.md`; the host engine's structure and the seams named below are in `.notes/rtx/openmw.md`.
This file is the route.

**Decisions and what is left, not a changelog.** §2–§5 are the choices everything else rests on; §6
is the route, one or two lines a milestone, marked; §7 is the tooling; §8 is what is not done. Code
comments cite this file by section number, so the numbering does not move.

---

## 1. What is being built

OpenMW keeps the game: cells, references, physics, scripts, animation, weather, the GUI. It stops
owning the picture. Primary visibility, shadows, direct and indirect light, sky, water, fog and
tonemapping are a Vulkan ray-tracing renderer, upscaled by DLSS Ray Reconstruction.

The renderer is **optional at compile time** (`-DOPENMW_RTX=ON`) and **off by default at runtime**
(`[RTX] enabled = false`). With the option off, not one line of it is compiled and the rest of the
build is byte-identical.

## 2. Where the scene comes from — the decision

Three ways to feed a ray tracer from this codebase.

| | what it means | verdict |
|---|---|---|
| **A. Read the ESM/NIF directly** | New cell streaming, animation, LOD, paging — rtxmw's route | **No.** That work is the reason to fork OpenMW instead. |
| **B. Mirror the OSG scene graph** | Keep OpenMW's whole content pipeline; traverse the live graph each frame and maintain a Vulkan mirror of it | **Yes.** |
| **C. Replace OSG's draw backend** | A Vulkan `osg::GraphicsContext` implementation | No. OSG's drawing model is fixed-function-shaped state assignment; ray tracing is not a draw call. |

**B, because of §3.3 and §3.5 of `.notes/rtx/openmw.md`**: the world is already CPU-resident triangles
with world transforms and resolved texture roles. The mirror is a traversal and a diff, not a content
pipeline.

Consequences, as they settled:

- **The mirror does not run downstream of a cull, and there is no cull.** Rays go everywhere, so a
  frustum has nothing to say about what must be reachable. The three things OpenMW hangs off its cull
  traversal are the renderer's own: rigs are posed by a visitor that culls nothing, and terrain LOD
  and object paging come from `Terrain::World::collect` asked by distance (`TerrainResidency`).
  Inheriting them was the defect — an actor outside the frustum mirrored in a stale pose, distant
  ground absent from reflections.
- **The mirror is incremental.** A full rebuild per frame will not hold a frame budget: instance
  transforms change every frame, geometry rarely, materials almost never. `SceneExtractor` is the
  shape that answers it and `SceneDesc` carries the argument — a placement is addressed by a slot it
  keeps for as long as it stands, so a world of fifty thousand placements and three hundred movers
  costs three hundred.
- **Draw-order tricks do not come across.** `RenderBin` ordering, the transparent-pass hack, the
  distortion pass, `pingpongcull` are rasterizer workarounds; the RT path answers the same questions
  with rays.

## 3. How the image reaches the screen

Two surfaces, one renderer, and no OpenGL under either.

**Headless (primary development surface).** A Vulkan device with **no surface extensions**, no
window, no GL. `openmw-rtxtool` loads game data, builds a scene, renders N frames, writes a PNG. It
works over ssh and starts in under a second warm, and it is where nearly all iteration happens.

**In-game: two paths, chosen at startup, and the one not chosen is never started.**
`-DOPENMW_RTX=ON` decides whether the ray tracer is built; `[RTX] enabled` decides whether it runs,
read once before the window exists. With it off the tree behaves exactly as upstream does. With it on
**OpenGL is not initialized at all** — no GL context, no `osgViewer` graphics window, no interop, no
rasterized frame underneath. The window is an SDL surface for Vulkan, the GUI is drawn by the
backend, and the inventory doll and the maps are traces rather than render-to-texture passes.

**Interop was the way in and is gone.** Vulkan rendered offscreen into an image exported with
`VK_KHR_external_memory_fd`, GL imported it and blitted it under the MyGUI overlay. That put a traced
frame on the screen at M2 while everything around it was simple. What it cost was a whole rasterized
world drawn and discarded every frame and a frame of latency (§12), and both went with it.

What the second path needed, and what each became: MyGUI a Vulkan render manager
(`components/myguirtx`); the doll and the maps traces into the renderer's own GUI texture table
(`MWRender::Rtx::TracedView`); video playback the same FFmpeg decode into a texture the backend
uploads (`MyGUIPlatform::Picture`); the window and swapchain already written since M0. **OSG stays as
a scene graph and a content loader** — the harness has proved since M0 that it needs no GL context to
be either.

## 4. Layout

```
components/rtx/                 openmw-rtx          the API-neutral core: no graphics API, no game
                                                    headers; osg::Node -> SceneDesc lives here too
    shaders/    GLSL + headers shared verbatim with C++ (`#ifdef __cplusplus`)
components/rtxvulkan/           openmw-rtx-vulkan   the Vulkan backend, and its GLSL
components/rtxmetal/            openmw-rtx-metal    the Metal backend (`.notes/rtx/backends.md`)
components/rtxbackends/         openmw-rtx-backends which backend a build has
components/myguirtx/            MyGUI's backend for it
components/surface/             what the content says a surface is; both renderers read it
apps/openmw/mwrender/rtx/       the game-side owner
apps/rtxtool/                   openmw-rtxtool      headless harness
files/rtx/views.cfg             named viewpoints; benches.cfg names the suites
```

C++ sources sit flat in each directory, as every other component in this tree does; only the GLSL
gets a subdirectory, matching `files/shaders`.

The seam that matters: **`openmw-rtx` never links a graphics API and never includes a game header.**
The linker holds the first half; nothing but taste holds the second. It is testable against synthetic
scenes with no game data present, which is what makes the allocation test and the pass tests fast and
hermetic.

**OpenSceneGraph was never that seam**, which is why the separate `openmw-rtx-bridge` folded back
in: the scene arrives as an `osg::Node` graph and the maths types are OSG's, so a wall between the
half that reads a graph and the half that does not had the same consumers on both sides and
duplicated code across itself. What a backend is handed is `SceneDesc`
either way.

Every shader is compiled by `glslc` at build time and **validated by `spirv-val` in the same custom
command** — an invalid module fails the build, not the frame. Debug builds compile with `-g` so
Nsight shows source.

## 5. Target hardware

Ada-class NVIDIA, and Apple silicon for the other backend. Confirmed available here:
`VK_KHR_ray_tracing_pipeline`, `VK_KHR_acceleration_structure`, `VK_KHR_ray_query`,
`VK_KHR_ray_tracing_position_fetch`, `VK_KHR_ray_tracing_maintenance1`, `VK_EXT_opacity_micromap`,
`VK_EXT/NV_ray_tracing_invocation_reorder`, `VK_NV_cluster_acceleration_structure`,
`VK_NV_partitioned_acceleration_structure`, `VK_EXT_mesh_shader`, `VK_KHR_cooperative_matrix`.

Required at startup; a missing one is a hard failure naming the extension, never a fallback path.

**§5.3 — the target this is all written against: 1920×1080 internal → 3840×2160 at 60 fps.**

## 6. Milestones

**M0–M11 are done. M12 is continuous.** What each settled that still binds is beside it; what any of
them left behind is in §8.

- **M0–M3 — device, mirror, visibility, textures. Done.** `SceneDesc` and a bridge that walks an
  `osg::Node` into meshes, instances, materials and lights; BLAS per mesh, TLAS per frame, a
  ray-query compute pass; one bindless texture array with DDS blocks passed through untouched.
  *Binds:* **ray-cone texture LOD went in from the start** — retrofitting it is how rtxmw lost a week
  of caustics (`design.md` §7.6).
- **M2b — terrain in the harness. Done.** An `ESMTerrain::Storage` over `EsmData::mLands` feeds a
  `Terrain::TerrainGrid`, one chunk grid per cell, no LOD — Seyda Neen traced at 5.5% before and 79%
  after, **and the renderer did not change at all**. *Binds:* a cell's blend map indexes the texture
  list of *the plugin that wrote it*, so the key is `(plugin, index)`; and `TerrainDrawable`'s
  one-pass-per-ground-texture stack is read back into layers and summed at the hit.
- **M4 — direct lighting and shadows. Done.** *Binds:* water is excluded from shadow rays **by a mask
  bit rather than a cutout test** — the any-hit version halves the frame rate; and everything about a
  lamp is derived, because a `LIGH` record carries a colour and a radius and no intensity.
- **M5 — sky, sun, moons. The dome is done; the weather in it is not** (§8). A gradient, the sun's
  disc, both moons, Morrowind's own cloud decks and its star sheet. Driven by `MWWorld::Weather` and `DateTimeManager` rather than re-derived from the
  ini. *Binds:* **sky is a light source, not a backdrop**, and the sun and the moons are discs a ray
  that hit nothing finds — so anything reflective gets them for nothing and their size lives in one
  place. And **the sun is one object, not the engine's five dials**: `Sky::sunAt` reads Morrowind's
  arithmetic, `Rtx::makeSkylight` is the only thing that may build a sun out of it, and its
  irradiance being zero is the whole of "there is no sun". Every sun bug this renderer has had was
  two of those dials disagreeing. **The clouds and the night sky are found rather than hung on a
  mesh**: the deck is where a ray crosses a layer at a height, and the night is read off the file the
  rasterizer draws it with — `Rtx::readNightSky` walks `Models/skynight01` at load and takes
  from it which sheet the star field wears, how much sky one tile of that sheet covers, the elevation
  the field fades out below, and where each of the six patches painted across it sits and how wide it
  is. Those patches are three nebulae and the warrior, the mage and the thief; they are most of what
  gives a Morrowind night its colour, and drawing the field alone puts stars on black. Each is the
  same disc a moon is and is drawn by the same arithmetic. **Nothing about that mesh is written down
  here**, because a mod replaces it and a table would go silently wrong. *Binds:* **`MWRender::RenderingManager` owns everything `WorldState` says about the
  sky**, off the `WeatherResult` it is handed, and `Sky::SkyRoll` is turned there too. Reading any of
  it back out of `SkyManager` is what made the game's sky read from a manager that may never have
  been built. **And the deck's crossing is `Sky::cloudBlend`**, which is where the black sky actually
  came from: the engine divides the transition by `Clouds_Maximum_Percent`, two of the ten weathers
  record none, and the NaN that fell out reached the shader from the game alone — the harness has no
  weather system to divide anything. A rasterizer survives one, because a NaN opacity draws nothing
  and the old sky stays; a tracer mixes its whole sky by it. Both now run one curve, and
  `describeClouds` makes the value well-formed at the boundary because a content file is untrusted.
- **M6 — water. Done.** TMA spectrum under Donelan–Banner spread, 32
  components, shortest wave 32 units; ripples carried on the swell; Schlick Fresnel with one
  reflection and one refraction ray **at the pixel's own cone spread, not the bounce spread**;
  Beer–Lambert with Jerlov coastal extinction; caustics from `1/|det J|` with the second-order
  correction that took the term from handing back 12.3% more light than fell on the water to 2.4%.
  *Binds:* **the shader is more current than any prose about it**, here and in
  `/home/xxorza/Projects/rtxmw/docs/design.md` §7; every expectation derives from `WATER_EXTINCTION`,
  `WATER_IOR` and `WATER_F0` rather than a literal beside it. And the surf is McCowan's breaking
  criterion against the depth *at this instant* — `H = 0.78 d`, so the band's width falls out of the
  sea state and nothing places it. **The note that said to take it off the sign of `det J` was
  wrong**: that Jacobian is the refracted bundle's, it scales with depth, and this surface is not
  displaced — so it is near the identity in exactly the shallows foam belongs in, and where it does
  invert it marks a caustic cusp under the water rather than a surface that folded.
- **M7 — fog. Done**, bar a test of a real view (§8). 24 non-uniform steps, density falling off from
  the cell's water level, three fbm octaves with a coverage band normalised by its own mean, HG-Draine
  at eight-micron droplets per steradian, eight shadow rays for shafts. *Binds:* **no new bindings** —
  folding fog into both halves inside the trace equals fogging their sum; and extinction comes from
  `MWRender::FogManager`'s own ramp read back rather than an eyeballed figure.
- **M8 — indirect light. Done.** Path-traced diffuse bounce, sky as an emitter, blue-noise sampling,
  à-trous denoise demodulated by albedo. Water and fog bypass the filter by construction.
- **M9 — de-lighting. Done.** An estimate divided out at sample time, judged on a contact sheet.
- **M10 — G-buffer and DLSS Ray Reconstruction. Done.** 1920×1080 internal → 3840×2160. *Binds:*
  **a ray that hits nothing still moves.** The sky is infinitely far, so the eye walking does not
  carry it — but the eye *turning* does, and that is most of what a player does. Storing nothing in
  the motion channel for a miss had the upscaler fetch the sky's history from the pixel it already
  occupied, and every turn of the head smeared it. A gradient hides that and a field of stars does
  not, which is how it was found; under a rotation the sky moves by exactly what a surface at any
  distance moves by, and there is a test that says so.
- **M11 — full in-game integration. Done.** Cells, objects, actors, camera, menus, screenshots, the
  local and global maps. *Binds:* **removal is mark and sweep over slots** — the mirror re-walks the
  whole graph every frame, so anything alive was met, and the identity maps are keyed on raw `osg`
  pointers an unloading cell can hand straight back for something else.
- **M12 — performance. Continuous** (§8).

### Where M12 stands

`openmw-rtxtool bench`, the `[default]` suite: 1920×1080 out of 1280×720 at DLSS quality, layers off,
600 frames of world at 60 Hz after a warm-up second, median of each row.

| place | frame | trace submit | place submit | fps |
|---|---|---|---|---|
| Seyda Neen's ship, 7,013 instances | 8.67 ms | 4.87 | 1.87 | 115.3 (77.9 at the 1% low) |
| Balmora's guild of mages, 1,239 | 6.79 ms | 5.00 | 1.14 | 147.4 (105.0) |

| place | trace | upscale | refit | tlas | **GPU total** |
|---|---|---|---|---|---|
| Seyda Neen's ship | 2.30 | 2.00 | 0.43 | 0.20 | **5.03 ms** |
| Balmora's guild | 2.21 | 2.07 | 0.27 | 0.17 | **4.81 ms** |

**The CPU and the device have converged.** Five milliseconds of device work inside an 8.67 ms frame,
where it used to be 8.74 inside 30. Both places are inside the 16.7 ms budget at 1080p, and the
remaining exterior cost is split evenly between the trace and the upscaler with nothing obviously
wasteful between them.

### What the finished features cost

Written down as each one lands rather than acted on — `CLAUDE.md`, *Feature-complete first, then
fast*. 1920×1080, no upscaling, validation off, best of thirty on this box.

| what | where | cost |
|---|---|---|
| the cloud deck and the stars | Seyda Neen's ship, a seventh of the frame sky | 8.06 → 8.02 ms |
| | straight up, all of it sky | 2.22 → 2.31 ms |
| shoreline foam | a shoreline in the middle distance | 8.14 → 8.25 ms |
| | the camera standing in the surf, foam over a third of the frame | 8.47 → 10.32 ms |
| the sprite layer, in the trace | Seyda Neen's ship, 165 emitters and 4,655 particles | 8.22 → 8.85 ms |
| | Balmora's guild of mages, 19 emitters | 7.34 → 7.47 ms |
| the harness's live props, whole frame | Balmora in a window, 94 props | 49 → 37 fps |
| a cell arriving | nineteen crossings across the island | none of them a rebuild (§10) |
| the GUI's own submit | 12,400 interface frames with a video playing | 0.38 queue round trips a frame |

The sky costs two texture reads on a ray that reached nothing and is inside the noise on a frame
with a world in it — the ship's pair differ by less than the run-to-run spread.

Foam's near-field figure is a shadow ray per covered pixel — it is lit the way every other diffuse
surface is — and the reflection and refraction rays under it are still traced and then mixed away
where it covers. Removing those would want the depth before the refraction that measures it, which
is the circle to break if it ever matters.

**Distant land is measured in memory rather than in milliseconds**, which is why it is not in the
table: what it costs is the size of the world, and the frame time that follows from that is M12's to
take against a finished frame. Balmora, `--distant-cells=4`, against the same view with none:

| what | textures | structures | triangles | scene build |
|---|---|---|---|---|
| no distance | 377 | 19 MiB | 199,821 | 577 ms |
| four cells of ground | 450 | 40 MiB | 502,157 | 2685 ms |
| and its statics | 619 | 165 MiB | 2,252,922 | 5822 ms |

The acceleration structure is where the statics land — four times over — because a merged chunk is
real geometry at full detail wherever no `_dist` mesh exists for it. Textures barely move against
that, since a distant building is made of the files the near cells already loaded. **73 composites,
28.5 ms each**, which is why the bake is drained sixteen rows a frame rather than done where it is
asked for.

The trace is the cheap half of the sprites and the sphere test is why: an emitter is a few units
across and one rejection throws it away for almost every pixel. The window's six milliseconds are not
the sprites — they are the 185 extra deforming drawables the instanced props bring, refit once a
frame whether or not their skeletons moved (§8).

## 7. Development infrastructure

This is not overhead; it is what decides how fast the milestones go. The verbs are in `CLAUDE.md`.

**7.1 The harness is the primary surface.** `shot` renders the real renderer headlessly in about a
second and prints hit fraction, camera and frame time, so a change is checkable without a screenshot
being looked at. `view` exists for what only a window shows: how something moves, and whether an
artefact is a still or a shimmer. `bench` counts seconds of **world**, not of wall clock — the world
steps a sixtieth a frame however long that frame took, so two builds render the same six hundred
frames with the same particles in the same places. The game is benched too, over savegames
(`apps/rtxtool/bench.sh`), because every renderer defect found in the M12 stretch was invisible in the
harness and obvious within seconds of measuring the game.

**7.2 Tests**, in the order they catch things: unit tests against synthetic scenes with no game data;
renderer tests driving the real renderer headlessly and asserting radiance; and `verify`, an A/B of
every view against **a directory this machine wrote earlier**, never a corpus in the tree — the
picture is a function of the driver and the card as much as of the code. `contactsheet` is for the
cases where the right answer is a judgement. Game data absent, the tests that need it **skip**;
present but wrong, they **fail** — a silent skip looks like a pass. Every test loads the validation
layers, synchronization validation included, and fails on any recorded error.

**7.3 Zero allocations per frame.** A steady-state frame with a stationary camera must not touch the
heap; the concern is jitter, not throughput. A counting global `operator new`/`delete` in one
translation unit of the test binary renders N frames of the real renderer and asserts a counter
across the measured window, against a named budget rather than `== 0` so a driver path that must
allocate can be accommodated deliberately. It measures zero. Forbidden on the frame path:
`std::string` construction, `push_back` into an unreserved vector, `std::function` capture,
`make_unique`, and any logging that is not compiled out.

**7.4 Validation and debugging.** Layers on in debug and in every test, error severity aborting after
logging with object names. Synchronization, GPU-assisted and best-practices validation opt in per
run. **Every Vulkan object is named** and every pass labelled with `VK_EXT_debug_utils`, compiled out
in release. `VK_EXT_device_fault` on device loss.

**7.5 Profiling.** Per-pass timestamp queries report the device's side; `apps/rtxtool/profile.sh`
wraps `perf` for the CPU's. Two things that would be re-learned otherwise: `task-clock` rather than
`cycles`, because this box's hybrid PMU splits `cycles` across `cpu_core` and `cpu_atom` and halves
every count; and a control fifo bounding the recording to the measured frames.

## 8. What is not done

**Content the renderer does not draw yet**

- **Weather effects** (M5's other half): rain, snow, ash and blight storms, blizzards. The sky over
  them is drawn — a weather's own cloud deck arrives with it and crosses on its own
  `Transition_Delta` — but nothing falls out of it.

  **It is one problem and not three, and reading the rasterizer is what says so.** This entry used to
  claim otherwise on all three counts:

  - *"A storm is mostly a medium"* — it is not, in this game. Ash, blight, snow and blizzard are each
    a NIF particle system hung on a camera-relative node (`Weather_<name>_Particle_Effect`), and rain
    is one `SkyManager` builds itself. Every one of them is `osgParticle`, which is the thing the
    sprite layer already reads. Drawing them as a medium would be a renderer this one does not have
    to be.
  - *"Lightning is a light"* — it is not one there either. `Weather::calculateThunder` decays a flash
    brightness and `calculateResult` adds it to the fog, the ambient and the sun colour alike
    (`weather.cpp:955`). **The ray tracer already gets it**, because all three of those reach it
    through `WorldState` with the flash folded in. Nothing to do.
  - *"The layer wants an oriented sprite before anything else"* — that part was right, and it is
    done. `osgParticle` offers `BILLBOARD`, whose axes are the screen's, and `FIXED`, whose are used
    exactly as authored; rain is `FIXED` with an X axis squashed to a tenth against a Y pointing
    straight down, which is a falling streak rather than a round drop. `GpuEmitter` carries those two
    axes, the sprite march meets a plane where they are set and a disc where they are not, and the
    emitter's sphere is measured on the quad's diagonal rather than its width — a streak ten times as
    tall as it is wide, measured on the width, is cut off nine tenths of the way up.

  **`MWRender::Precipitation` is done and the reach is wired.** The rain system and the storm effect
  node came out of `mwrender/gl/sky.cpp` into a renderer-neutral home; `SkyManager` keeps only what
  is genuinely the rasterizer's — the occlusion pass, the underwater cull callback, the shader hints
  — and puts them back through `getRevision`, which counts the rebuilds. `RenderingManager` hands the
  node down on `WorldState` and `RtxRenderer` walks it as a second root, with the eye added back
  because those nodes hang beneath the sky's camera-relative transform.

  **The mask that walk was given was wrong, and what it exposed was much larger than rain.**

  - `Mask_WeatherParticles` selected almost nothing under it. `Resource::SceneManager` stamps
    `Mask_ParticleSystem` on the `ParticleSystem` drawable of every model it loads, so a blizzard's
    own particles are not marked as weather: the storm was extracted with all of its particles
    missing. The walk begins at the precipitation node, where everything below is precipitation by
    construction, so it takes the lot.
  - The rain's state set is assembled by hand and so was never *described*: `ShaderVisitor` augments
    a `Surface::Material` and does not invent one, and the extractor reads the description rather
    than the texture attribute. Everything else the weather throws comes out of a NIF and the content
    pipeline describes it. `Precipitation` now says what that surface is in `Surface`'s own terms.
  - **`osgParticle` runs its entire simulation from the cull traversal, and this renderer has none.**
    Emission, the affector programs and the integration all live in `ParticleProcessor::traverse` and
    `ParticleSystemUpdater::traverse`, and both open by asking whether the visitor calling them is a
    cull visitor. So nothing had ever stepped a particle in the ray-traced path — not the rain, and
    not **any** particle system in the game: candles without flames, braziers without smoke, every
    one running on the seed its file was authored with. It fails silently, because the emitter is
    still in the scene and still places sprites; they simply never change.

  **The mirror walk owns it, because the mirror walk is already what a cull traversal used to do
  here** — it is what poses an actor, and it is the only thing that reaches every emitter. It claims
  the cull visitor's name at the two nodes that ask and for the length of one call. `PoseCull` is the
  real one and warns against exactly this, because `SceneUtil::RigGeometry`, `MorphGeometry` and
  `MWRender::CameraRelativeTransform` all answer that question with an unchecked `static_cast`;
  neither of these two casts, both only compare, and `ParticleSystem::update` reaches for the visitor
  through the checked `asCullVisitor` — which answers null and skips a depth sort a ray tracer has no
  use for. Both derive from a plain `osg::Node` whose `traverse` is empty, so the claim cannot reach
  a child, and that is what keeps it away from the three that would take it badly.

  **One emitter clock, and nothing else in the renderer may drive a particle.** `osgParticle` keeps a
  once-per-frame guard and a `_t0` per processor, so two visitors stepping the same emitter on two
  clocks is not two steps — it is a `_t0` from whichever wrote last, differenced against the other's
  simulation time, and every plume in the cell on its own ceiling. The extractor owns that clock; it
  moves only through `advanceEmitters`, which cannot be handed a jump or a step backwards, and its
  frame number is the single sequence that guard is kept against, so however many walks reach an
  emitter exactly one of them steps it. `PoseCull` is a real cull visitor and so was a second driver
  in the harness; it now leaves particle nodes alone. The warm-up that fills a cell's candles before
  the first `shot` runs on the same clock, through `stepEmitters`.

  Two more, found on the way and fixed at the source rather than worked around:

  - **Freeze-on-cull.** `ParticleSystem::_last_frame` advances in `drawImplementation` and nowhere
    else, so with no OpenGL draw every system would be judged off screen two frames in and stopped
    for good. Cleared where a system is driven, because it is a fact about this renderer rather than
    about any emitter.
  - **The underwater gate came from the cull.** `SkyManager` froze the rain from
    `UnderwaterSwitchCallback`, which reads a view point only a cull traversal writes — under the ray
    tracer it never runs, so what it last saw is the origin and the rain kept falling to the sea bed.
    `Precipitation` now asks the camera, which both renderers keep, and the RTX walk skips the
    subtree entirely where the rasterizer's cull callback would have hidden it.

  The rain's updater also moved above the system it drives, which is where `NifOsg` deliberately puts
  it: a walk meeting the updater first reads a system already integrated this frame rather than one
  frame of staleness. The rasterizer cannot tell, since it bins the drawable and draws it later.

  **Measured at Seyda Neen, weather forced from the console.** Clear: 21 emitters, 361 sprites,
  6.1 ms. Rain: 22 emitters, ~1650 sprites, 13.0 ms — and both counts move every report, which is
  what says the simulation is running rather than frozen. That the rain roughly doubles the frame is
  a number for M12 and is written down here rather than acted on. Three tests pin the mechanism:
  without the cull claim they report zero particles ever created, which was exactly the symptom.

  **Then the four of them turned out to be one.** Three bugs in one subsystem, every one of them
  found by launching the game — that is a signature, and what it names is that precipitation lived
  under `apps/openmw/` and so was the one part of a frame the harness could not build. The harness is
  how a rendering change is meant to be checked here, so the one part it could not reach is the one
  part where a wrong mask, a gate read off a cull and an emitter nothing stepped all survived.

  So it moved. `Weather::Precipitation` is in `components/weather/`, with `Weather::Downpour` — what
  falls, how hard, how fast the wind drives it — as its input instead of the game's whole answer
  about the sky, and `Weather::downpourAt` reading that off the content files for anyone with no
  weather system to run. `StagedWorld` owns one, `--weather` feeds it, and `openmw-rtxtool scene
  --weather=Rain` now reports the same thousand-odd drops the game does. Every weather that drops
  anything is checkable in about a second, without a window.

  Two things it needed on the way, both the same shape as the bugs themselves: it reached into an
  `osg::Camera` for the eye, and it kept a water level of its own. Both are facts the caller already
  has — `MWRender::Camera` and `MWRender::Water` in the game, a viewpoint and a cell's water level in
  the harness — so both are handed over now. `WorldState::mUnderwater` already carried the second one
  a line above where the precipitation node was handed over. `Weather::stormEffect` is the table of
  which weather drives which model; the game's weather manager reads it too, rather than keeping a
  second copy of four entries.

  **And the mask and the ordering, structurally rather than by correction.**

  - A node mask is AND-ed at every node on the way down, so it can only ever mean "which categories
    may be seen at all" — a different question from "which subtree am I walking", which is answered
    by where the walk starts. The weather walk now takes the same exclusion every other walk takes,
    and the set-and-restore around it is gone: that dance was the shape the mistake arrived in.
    (An inclusion mask is still right for a camera — the local map wants exactly five categories —
    which is why this is a rule about walks that choose a root, not about masks.)
  - Sprites are read after the walk has settled rather than as it passes each system, so where an
    updater sits among its siblings stops being a question. That one was worse than it looked:
    reading before integration does not lag a position, it drops the sprite entirely, because a
    particle's interpolated size and alpha only exist after `Particle::update`. An updater below its
    system meant no rain at all, not late rain.

  Four tests pin the mechanism, each checked against the code it guards: without the cull claim they
  report zero particles ever created; without the deferral the wrongly-ordered graph places nothing.

  **What is left:** nothing named here. The harness renders every weather; the game agrees with it.

  **What was left before this:** Those nodes hang under `SkyManager`'s `Mask_Sky` root,
  which the extractor's traversal mask excludes, under a `CameraRelativeTransform` that strips the
  translation — so the walk needs the eye added back and the mask opened. And the construction itself
  is in `apps/openmw/mwrender/gl/`, which is the wrong home for something both renderers draw: it
  wants a renderer-neutral `MWRender::Precipitation` that owns the rain system and the effect node,
  with `SkyManager` keeping only what is actually the rasterizer's — the occlusion pass, the
  underwater cull callback and the shader hints. Rebuilding the particle systems on the ray tracer's
  side instead is the one thing not to do; they are the same `osgParticle` objects and there should
  go on being one of each.


**Performance — M12, and none of it is started**

- **SER** (`VK_EXT_ray_tracing_invocation_reorder`). **Needs a ray-tracing pipeline**, because
  `reorderThreadEXT` exists in ray generation shaders only — but it does *not* need payloads, hit
  shaders or a real binding table: a `.rgen` can hold `rayQueryEXT` and `reorderThreadEXT` together,
  which was compiled and checked — the SPIR-V carries `OpCapability RayQueryKHR` and
  `OpReorderThreadWithHintEXT` side by side. So the megakernel body survives the move and what
  changes is the stage, a one-group pipeline and `vkCmdTraceRaysKHR`. `VK_KHR_ray_tracing_pipeline`
  is already required and its three entry points are already loaded in `Device`. The device reports
  `reorders` rather than `none`, which `openmw-rtxtool info` prints.

  **Deferred, and the argument for it is weaker than it looked.** The occupancy case is gone —
  `visibility.comp` runs at 50%, bound equally by 80 registers and by the 8,448 bytes the ray query
  traversal stack costs, so shrinking live state does not move it. What is left is coherence, and
  this frame is largely screen-coherent — a claim nobody has measured — and on top of that SER costs
  in proportion to the live state carried across the reorder, which this kernel has a great deal of.

  **What would settle it is a day**: port `visibility.comp` to a raygen shader with *no* reorder call
  at all, hold the byte comparison, and compare registers, occupancy and the trace timer. The risk is
  the port and not the reorder — a raygen shader has a different scheduling and stack model, and a
  288 KB megakernel may spill where it does not today. If the port alone regresses, the reorder never
  has to be argued.
- **Opacity micromaps for cutout foliage.** The device features are required and probed; nothing
  builds a micromap.
- **BLAS compaction**, and **cluster acceleration structures** if they earn their place.
- **Refit only what moved.** A prop's emitters change every frame and its geometry usually does not,
  and nothing tells the two apart — 185 deforming drawables refit every frame in the harness's props
  view.
- **The GUI's second submit.** `GuiTextures` flushes just before `drawGui` rather than inside it, so
  a frame that writes a texture costs two queue round trips where it could cost one. Measured at 0.38
  a frame, which is not what a frame is spending its time on.

**Content**


- **Solstheim's two cloud decks are not loaded.** `files/openmw.cfg` names them `Tx_Sky_Snow.dds` and
  `Tx_Sky_Blizzard.dds`, and what Bloodmoon ships is `tx_bm_sky_snow.dds` and `tx_bm_sky_blizzard.dds`
  — so snow and blizzard get no deck at all rather than the wrong one, which is what
  `addSkyTextures` checks the archives for. The rasterizer reads the same fallbacks and has the same
  gap, so this is a line of a real `Morrowind.ini` rather than anything either renderer does.

**Tooling**

- **A test of a real fogged view** (M7). The banks over a shore and the shafts themselves are looked
  at rather than asserted; there is no histogram test.

**Open questions**

- **Interiors.** A room is not a valley (`design.md` §8.42) and interiors are half the game. Whether
  fog, sky light and bounce need a separate interior model is unanswered.
- **Groundcover.** Grass is alpha-cutout and enormous in instance count. Particles were the other
  half of this question and are answered: a sprite goes in a table beside the lights and is marched
  against the primary ray, never into an acceleration structure.
- **Distant land — built, and none of it tuned.** The feature landed whole: ground past a cell is
  flattened into one baked texture on the CPU, distant statics arrive through
  `Terrain::ObjectPaging` in both worlds, and `[RTX] distant land cells` is the one number the
  ground and the air are both measured against. What distance is *right* is still unmeasured, and
  so is everything the bake trades:
  - **How large a composite should be.** `sCompositeExtent` is 512 and one costs 28.5 ms; the cost
    falls with the square, so 256 is a quarter of it and a blurrier hillside. Nothing has looked at
    where that trades.
  - **How many layers a large chunk's stack has** once composites are off. `createPasses` gathers
    what the area holds; twenty passes on an eight-cell chunk would not break the bake but would
    make the shading before it lands expensive enough to want a coarser ancestor instead.
  - **A composite is baked at one delight strength and then cached**, because the painted light has
    to come off per tile while the tiling is still known. Changing `delight` at runtime therefore
    leaves every standing composite at the old strength — whether that wants a rebake or wants the
    setting to stop being live is unanswered.
  - **Groundcover is the third chunk manager and neither world registers it.** It wants its own
    distance and probably its own answer; see the entry above.

- **The harness's people are a posed row, not the game's.** `PosedActors` stands creatures and NPCs
  in front of the camera and steps their animation by a clock, which exercises skinning, rigs and the
  deforming path and does not exercise what the mechanics actually ask for — an idle chosen by AI, a
  weapon drawn mid-swing, someone turning to face the player. A pose defect only the game produces
  cannot be reproduced under `shot`. Probably never fully closed: what an NPC is doing is a mechanics
  answer, and the harness has no mechanics.

**Standing risk**

- **Vanilla content assumes a rasterizer** — sheets lit from both sides, discarded outer transforms,
  Z-first Euler angles, two-sided stencil (`design.md` §8.1–8.6). Every one is diagnosed next door.
  Read it before debugging anything that looks like a content bug.

## 9.

Risks and open questions were here and are in §8, where what is left is. The number stays empty
because §10 and §12 are cited from code.

## 10. Slots, not compaction

**Done, and it is the rule the tables are built on.** `SceneDesc` used to reclaim by compacting, and
that is why a cell boundary cost a full `setScene` — nineteen of nineteen crossings on a route across
Vvardenfell, because a departing cell almost always takes a mesh or a material with it and
renumbering invalidates every acceleration structure and the whole texture array.

**Nothing is renumbered while the world is being walked.** A mesh, material or texture that nothing
names is freed — its index goes on a free list and the entry stays where it is — and the next arrival
that fits takes the slot, counting as an arrival because it holds different content than it did.
Travel is the exception and the one the player already pays for: `clear()` replaces the scene.

**A slot and the room behind it are two different things.** A slot is one row of a table and every row
is the same size; what varies is the run of vertices, indices, layers or mask weights it points at,
and that is handed out by `Rtx::SpanAllocator` — best fit, because the free list is what a departing
ring left and first fit spends a cathedral's hole on a crate, with freed runs merged so a cell's
thousands of releases become the one hole it arrived as. On the device the same run lives in
`BlockedBuffer`: blocks allocated once and never moved, so a bottom-level structure's address stays
valid and a crossing appends instead of rebuilding.

Flying the Bitter Coast to the Ashlands, nineteen boundaries in ten seconds of world:

| | compacting | slots |
|---|---|---|
| crossings that rebuilt everything | 19 of 19 | **0 of 19** |
| worst frame | 2,808 ms | **547 ms** |
| crossings, total | 39.7 s | **7.3 s** |
| — of that, building | 28.5 s | **0.9 s** |

**One bug came with it and is worth remembering.** `VulkanRenderer` decided whether to rebuild by
comparing the mesh table's *size*, which cannot see a freed slot taken over by something else. A
skinned body landing in one was refitted into a structure never made for it — reached after seventeen
crossings. The tables carry revisions that count arrivals rather than entries.

**Ownership is counted, not swept.** A texture is freed when the last material or hold naming it lets
go, wherever that happens; the sweep only ever answered on the frames a mesh or a material died as
well.

## 11. The three roots — closed

`.notes/ISSUES.md` once listed five defects that traced back to three roots, and all three are shut.
**A**, variable-length runs with no allocator, is `SpanAllocator` for the host runs and `BlockedBuffer`
for the device geometry. **B**, the mirror reading outside cull where `SceneUtil::StateSetUpdater`
only writes during one, is `SceneExtractor::animate` running the animators itself against a state set
it keeps per node, with `Material::mTextureTransform` for the UV controllers that had nowhere to land.
**C**, arrivals reported as a count rather than a set, is a slot carried on every arrival — for
textures and for meshes both.

## 12. The seam with the rasterizer — closed

There was one, and code comments still point here for what it was. The rasterizer drew the whole
world every frame into a framebuffer nobody looked at; `Tracer`'s composite went over the top inside
the same traversal, so the image presented was the *previous* frame's trace. It cost 2.05 ms a frame
of drawing at Seyda Neen against 6.2 ms of tracing, plus a frame of latency — and, worse than either,
it made skinning, terrain LOD and object paging view-dependent decisions inherited from a cull.

**What closed it was owning those three** (§2) and then not needing the traversal at all. There is no
cull, no interop and no rasterized frame underneath: the trace runs inside the frame that presents it.
The harness was the existence proof throughout — it has no cull, poses its actors by hand, and renders
the same content correctly.
