# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A fork of OpenMW 0.52 whose purpose is an **experimental ray-traced renderer**. Upstream OpenMW is
the host engine — cells, references, physics, scripts, animation, weather, GUI — and it keeps all of
that. What it stops owning is the picture.

Three documents, and they do not overlap:

- **`.notes/rtx/openmw.md`** — how the host engine is built and where the seams are. Read before touching
  anything in `apps/openmw/mwrender/`, `components/sceneutil/`, `components/resource/` or the
  settings plumbing.
- **`.notes/rtx/plan.md`** — the route: the scene-mirroring decision, the milestones, the tooling.
- **this file** — goals and working rules. Status goes in commits, never here.

The reference implementation is **`/home/xxorza/Projects/rtxmw/`** — a Rust Morrowind ray tracer
with working water, caustics and volumetric fog. Its `docs/design.md` is 3,400 lines of accumulated
findings, most of which are about *Morrowind's content*, not about Rust: ray offsets on sheet
geometry, discarded outermost transforms, Z-first Euler angles, the pre-lit albedo problem, DLSS
parameter traps. **Read the relevant section before debugging something that looks already solved.**
Its shaders are more current than its prose. Its licence is MIT OR Apache-2.0 and it is the same
author's; this fork is GPLv3.

## Posture

A 2002 game made to look astonishing on current hardware — ray-traced visibility, path-traced
indirect light, materials recovered from pre-lit vanilla textures, DLSS Ray Reconstruction, opacity
micromaps, SER. Vanilla content, new light transport.

Priorities, in order:

1. **How it looks.** Trading image quality for simplicity or convenience is the wrong trade.
2. **Performance.** 1920×1080 internal → 3840×2160 at 60 fps (`.notes/rtx/plan.md` §5).

Nothing else ranks: no mod compatibility, no configurability for its own sake, no portability layer,
no abstraction over hardware this does not target.

**Feature-complete first, then fast.** That order is about sequencing and not about the two above:
performance still loses to how it looks, and it also waits. Until the renderer draws everything the
game has, an optimisation is aimed at a frame that is about to change shape — the thing made cheap
turns out not to be the expensive one once the missing half arrives, and the measurement it was
justified by has to be taken again anyway.

So: land what is missing, **measure what it costs the moment it lands**, and write the number down
rather than acting on it. That list is what M12 is for. The exception is a cost so large it stops
the work — something that makes the harness too slow to look at, or a frame too slow to judge — and
that is a judgement to state out loud, not a licence.

Sports programming — strongest technique over safest, fast path first, delete what stopped earning
its place, settle arguments by measuring. Nothing here is published, so rewriting beats working
around.

### What that means against upstream

Upstream's constraints are not ours. Where they conflict, ours win.

- **Two renderers in one binary, one of them chosen at startup — and the other is then never
  started.** `-DOPENMW_RTX=ON` decides whether the ray tracer is *built*; `[RTX] enabled` decides
  whether it *runs*, read once before the window exists. Not a refactor of the existing renderer, not
  a strategy pattern bolted onto `RenderingManager`.

  **With it on, OpenGL is not initialized at all** — no GL context, no `osgViewer` graphics window,
  no interop, no rasterized frame underneath. The window is an SDL surface for Vulkan, the GUI is
  drawn by Vulkan, and the inventory doll and the maps are traces rather than render-to-texture
  passes. OSG stays, as a scene graph and a content loader; `openmw-rtxtool` has proved since M0
  that it needs no GL context to be either.

  **With it off, the tree behaves exactly as upstream does.** The rasterizer is not modified, not
  wrapped and not conditionally compiled around — it is simply the path not taken. Keeping it that
  way is what makes "does the RT path do this correctly" answerable by comparison.
- **No merge-back discipline.** This fork is not upstreaming. Do not shape a change around what a
  GitLab reviewer would accept.
- **Rasterizer workarounds do not come across.** Render-bin ordering, the transparent pass, the
  distortion pass, shadow-map tuning — the RT path answers those questions with rays.
- **A missing extension or feature is a hard failure naming it**, never a fallback path.

### Two renderers

The picture is reached twice — **Vulkan on Ada-class NVIDIA, Metal on Apple silicon** — as two
backends behind one API-neutral core, not a portability layer over either (`.notes/rtx/backends.md`).

Content, light transport and what the scene *is* live in the core, written once. What is true of an
API lives in its backend, written twice, and that cost is paid rather than abstracted away.

**Each machine develops its own renderer and leaves the other alone.** The backend this box cannot
run is not built, tested or debugged here; its skipped tests are the result, not a gap to close. The
core is the exception — a mistake there is one nobody here can see.

## Commands

Configure once. Turning off the Qt tools cuts the build roughly in half:

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DOPENMW_RTX=ON \
  -DOPENMW_DLSS_SDK=/home/xxorza/Projects/rtxmw/.refs/dlss \
  -DBUILD_COMPONENTS_TESTS=ON -DBUILD_OPENMW_TESTS=ON \
  -DBUILD_OPENCS=OFF -DBUILD_WIZARD=OFF -DBUILD_ESSIMPORTER=OFF \
  -DBUILD_MWINIIMPORTER=OFF -DBUILD_OPENCS_TESTS=OFF \
  -DOPENMW_USE_SYSTEM_RECASTNAVIGATION=ON -DOPENMW_USE_SYSTEM_GOOGLETEST=ON \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=mold
```

If it ever has to be configured again: **`bullet-dp`, not `bullet`** — OpenMW needs a
double-precision Bullet, the two Arch packages conflict, and the single-precision one has to come out
first. Targets are `openmw`, `openmw-rtxtool` (the usual inner loop), `components-tests`,
`openmw-tests`.

**Never `cmake --build --clean-first`.** Upstream declares `files/lang/*.ts` — translation files in
the *source* tree, with thousands of human translations in them — as build byproducts of the
`translations` target. Cleaning deletes them, and the rebuild regenerates them from scratch with
`lupdate`, so every translation becomes `type="unfinished"`. `git checkout -- files/lang/` puts them
back. Delete the build directory instead if a clean build is really wanted.

Tests are gtest binaries run directly — `./build/components-tests`, `./build/openmw-tests`, with
`--gtest_filter`. **There is no ctest registration in this project.**

Formatting and the other CI gates:

```sh
CLANG_FORMAT=clang-format-14 CI/check_clang_format.sh   # CI pins 14; this box has 22, they differ
CI/check_file_names.sh
CI/check_cmake_format.sh
clang-format -i <file>...
```

Running things:

```sh
cd build                       # --resources defaults to ./resources, so the tool runs from here

./openmw-rtxtool                                   # a window on the ship at Seyda Neen
./openmw-rtxtool view --view=balmora               # a window somewhere else; F1 lists its keys
./openmw-rtxtool shot --view=balmora --out=b.png   # one frame, no window — the default way to look
./openmw-rtxtool scene --view=balmora --twice      # what the renderer was handed
./openmw-rtxtool doll --npc=fargoth --out=d.png    # the inventory doll, traced headlessly
./openmw-rtxtool map --view=balmora --out=m.png    # one local-map tile, straight down
./openmw-rtxtool scene --list-views                # the named viewpoints
./openmw-rtxtool info                              # the device and its ray tracing limits

./openmw --skip-menu --new-game --start "Seyda Neen, Census and Excise Office"
```

`~/.config/openmw/openmw.cfg` points at the Morrowind install (GOTY, `/home/Games/Morrowind`), so
none of these needs `--data`; views live in `files/rtx/views.cfg`. Tests that need game data **skip**
when it is absent and **fail** when the path is set and wrong — a silent skip looks like a pass.

### Verification, after changing code and before saying it works

Build the targets you touched, run the test binary that covers them with a filter, then format.
Building the world to check a one-line change in the harness is waste; so is claiming a change works
because it compiled.

**Do not open the game window to check a rendering change.** `openmw-rtxtool shot` renders the real
renderer headlessly in about a second and prints a summary line — hit fraction, camera, frame time —
so a change is checkable without a screenshot ever being looked at.

**Do not bench, and do not report frame times, until the renderer draws everything the game has.**
That is *Feature-complete first, then fast* taken to its conclusion: a suite run against a frame that
is about to grow a sky, weather and groundcover measures a frame nobody will ever see, and the number
it produces has to be taken again anyway. Land the feature, check it with `shot`, and move on — the
figure a landing feature costs gets measured in M12, in one pass, against the finished frame. The
exception stands and is the only one: a cost so large it stops the work, which is a judgement to say
out loud rather than a licence to go measuring.

`openmw-rtxtool view` opens a window, which is for the things only a window shows: how something
moves, how it holds up while you fly through it, whether an artefact is a still or a shimmer. It
takes `--frames N` so it can also be run by something that cannot click — which is how the window
path gets exercised under the validation layers.

In the window, **P** prints the camera as a `views.cfg` block and **F3** prints the whole frame as a
command line. A file of those lines is a profiling corpus: each one renders that frame again under
`shot`.

## Architecture, in one screen

OpenMW is OpenSceneGraph 3.6 on OpenGL. `Engine::frame` (`apps/openmw/engine.cpp:93`) runs
simulation, then `mRenderer->updateTraversal()`, then `RenderingManager::renderFrame()`.

**The seam is `MWRender::Renderer`** (`apps/openmw/mwrender/renderer.hpp`): one image of the world on
the screen and the window it goes in, chosen once by name before the window exists. `GlRenderer`
(`mwrender/gl/`) is upstream's viewer, gathered rather than modified; `Rtx::RtxRenderer`
(`mwrender/rtx/`) owns an SDL surface with no GL context anywhere under it. Beside it sit
`MWRender::Stage` — the frame stamp, camera, event queue and stats that `osgViewer::Viewer` used to
bundle with a graphics context — and `SceneFrame`/`WorldState`, which is what the world says it is
doing this frame, in its own numbers, handed down once.

`MWRender::RenderingManager` (`mwrender/renderingmanager.hpp`) is still the rendering god-object,
owned by `MWWorld::World`, and it is now what describes the frame rather than what draws it.

The RT renderer takes its scene by **mirroring the live OSG scene graph**, because the world is
already CPU-resident triangles with world transforms, resolved texture roles and — for actors —
skinned vertex positions. There is no cull under it: rays go everywhere, so the mirror walks the
whole graph and poses what it finds itself. It reaches the screen through **its own swapchain, not
interop** — the GUI is drawn by the backend, and the inventory doll and the maps are traces rather
than render-to-texture passes. `.notes/rtx/plan.md` §2–3 has the alternatives.

Code lives in `components/rtx/` (the API-neutral core: what the scene *is*, `osg::Node` → scene
description, and no graphics API and no game headers), `components/rtxvulkan/` and
`components/rtxmetal/` (the two backends, picked by `components/rtxbackends/`),
`components/myguirtx/` (MyGUI's backend for it), `apps/openmw/mwrender/rtx/` (the game-side owner)
and `apps/rtxtool/` (the headless harness). `components/surface/` is renderer-neutral and holds what
the content says a surface is, which both renderers read.

### The frame, in the order it happens

`Renderer::renderFrame` is one recording on one queue, waited on before it returns — **one frame in
flight**, which is why the host tables are written straight into device memory with no staging and
no double buffering. In order: **visibility** (the trace: one ray per pixel, one bounce, writing ten
G-buffer channels), then either **accumulate → à-trous** or nothing at all, then **composite**
(`direct + albedo * indirect`), then **DLSS**, then **exposure** (a histogram of the frame the curve
is about to map) and **tone**, then the presenter blits to the swapchain.

**The two reconstruction paths are exclusive and `Rtx::Reconstruction::resolve` is the only copy of
the rule**: with any `upscale` other than `off` the denoiser is Ray Reconstruction and the wavelet
does not run; with `upscale = off` it is the à-trous cascade over the accumulator, or nothing. The
shipped default is `upscale = quality`, so in the game the accumulator and the cascade are *dead
code paths* — a habit worth keeping, because half the frame's temporal machinery is off.

Both the game and the harness **walk the whole graph every frame** — `clearPlacement()` empties the
lights, sprites and emitters, the walk refills them, and `SceneUploader::hand` takes the cheapest of
place / extend / rebuild. So anything the walk fails to reach is gone for that frame and back on the
next; the counts in `Rtx::ExtractionStats` are what say whether that happened.

### Which tool answers which question

`shot` is the default: one camera, `--repeat` traces of it, a PNG and a summary line — and it takes
`--pos`/`--look`/`--hour`/`--exposure`, so a hypothesis about one frame can usually be settled with
two of them and no window. `scene` answers "what was the renderer handed" without drawing. `bench`
is the only headless path with a **moving** camera: a view with `to` and `speed` flies a route, and
it re-walks and re-hands the scene every frame exactly as the game does, so it is what reproduces
anything that depends on motion or on cells arriving. `view` is for what only a window shows, and
`--frames N` lets something that cannot click drive it. `info` prints the device's ray tracing
limits.

## Conventions

**C++20, `.clang-format` at 120 columns.** The user's global Rust rules do not apply to this tree;
the posture behind them does.

- **Comments say *why*.** A block comment on a type or a non-obvious function says what it is for.
  Inside a body, a comment earns its place by naming an invariant, a workaround and its cause, or a
  trade-off against the obvious alternative — never by restating the line under it. No decorative
  dividers.
- **Fix stale narration in code you are already editing**, like fixing indentation on a line you are
  changing. Sweeping files you are not otherwise in is a separate task.
- **Frame times are uniform, and an average that hides a spike is not an answer.** A cost that is
  cheap on most frames and enormous on one is worse than the same total spread evenly: the spike is
  a dropped frame and a visible hitch, and no amount of amortising makes it not one. So work is made
  *incremental*, never *batched behind a threshold* — a table grows and recycles its slots rather
  than being compacted when enough of it has died, and a resource is appended rather than rebuilt
  when it changes. If an operation cannot be made cheap, it belongs off the frame path entirely, not
  on a rota. Report the p99 and the worst frame beside the median, because those are the ones a
  player feels.
- **Allocation is a metric on the frame path.** Persistent scratch buffers refilled with `clear()`,
  results into an out-parameter, nothing that constructs a `std::string` or a `std::function` per
  frame, logging that compiles out. There is a test that enforces this (`.notes/rtx/plan.md` §7.3).
- **Asserts** guard contracts the code must keep, not data the world might supply. Hot paths use the
  debug-only form; untrusted input is never an assert.
