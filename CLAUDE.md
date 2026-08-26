# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A fork of OpenMW 0.52 whose purpose is an **experimental ray-traced renderer**. Upstream OpenMW is
the host engine — cells, references, physics, scripts, animation, weather, GUI — and it keeps all of
that. What it stops owning is the picture.

Three documents, and they do not overlap:

- **`.notes/rtx/openmw.md`** — how the host engine is built and where the seams are. Read before
  touching anything in `apps/openmw/mwrender/`, `components/sceneutil/`, `components/resource/` or
  the settings plumbing.
- **`.notes/rtx/plan.md`** — the route: the scene-mirroring decision, the milestones, the tooling.
- **this file** — goals and working rules. Anything that the tree, `--help` or a commit already
  answers does not belong here.

The reference implementation is **`/home/xxorza/Projects/rtxmw/`** — a Rust Morrowind ray tracer
with working water, caustics and volumetric fog. Its `docs/design.md` is a long accumulation of
findings that are mostly about *Morrowind's content* rather than about Rust: ray offsets on sheet
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

**Feature-complete first, then fast.** Until the renderer draws everything the game has, an
optimisation is aimed at a frame that is about to change shape, and the measurement justifying it
has to be taken again anyway. So: land what is missing, note what it costs, and act on the number
later. The exception is a cost so large it stops the work — a harness too slow to look at, a frame
too slow to judge — and that is a judgement to state out loud, not a licence.

Sports programming — strongest technique over safest, fast path first, delete what stopped earning
its place, settle arguments by measuring. Nothing here is published, so rewriting beats working
around.

### What that means against upstream

Upstream's constraints are not ours. Where they conflict, ours win.

- **Two renderers in one binary, one of them chosen at startup — and the other is then never
  started.** `-DOPENMW_RTX=ON` decides whether the ray tracer is *built*; `[RTX] enabled` decides
  whether it *runs*, read once before the window exists. Not a refactor of the existing renderer,
  not a strategy pattern bolted onto `RenderingManager`.

  **With it on, OpenGL is not initialized at all** — no GL context, no `osgViewer` graphics window,
  no interop, no rasterized frame underneath. The window is an SDL surface for Vulkan, the GUI is
  drawn by Vulkan, and the inventory doll and the maps are traces rather than render-to-texture
  passes. OSG stays, as a scene graph and a content loader.

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

Content, light transport and what the scene *is* live in the core, written once. The core carries no
graphics API and no game headers. What is true of an API lives in its backend, written twice, and
that cost is paid rather than abstracted away.

**Each machine develops its own renderer and leaves the other alone.** The backend this box cannot
run is not built, tested or debugged here; its skipped tests are the result, not a gap to close. The
core is the exception — a mistake there is one nobody here can see.

### Where it lives, and what a frame does

`components/rtx/` is the core — scene description, no graphics API, no game headers.
`components/rtxvulkan/` and `components/rtxmetal/` are the backends, picked by
`components/rtxbackends/`; `components/myguirtx/` is MyGUI's backend, `components/surface/` is what
the content says a surface is. `apps/openmw/mwrender/rtx/` is the game-side owner, `apps/rtxtool/`
the harness, and `MWRender::Renderer` the seam against `mwrender/gl/`.

## Traps

The build is already configured in `build/`; `openmw-rtxtool --help` lists the harness, and the
`CI/check_*.sh` scripts are the gates. What those do not tell you:

- **`bullet-dp`, not `bullet`**, if it ever has to be configured again — OpenMW needs a
  double-precision Bullet, the two Arch packages conflict, and the single-precision one has to come
  out first.
- **Never `cmake --build --clean-first`.** Upstream declares `files/lang/*.ts` — translation files in
  the *source* tree, with thousands of human translations in them — as build byproducts of the
  `translations` target, so cleaning deletes them and the rebuild regenerates every translation as
  `type="unfinished"`. `git checkout -- files/lang/` puts them back. Delete the build directory
  instead if a clean build is really wanted.
- **CI pins clang-format 14**; this box has 22 and they disagree, so run
  `CLANG_FORMAT=clang-format-14 CI/check_clang_format.sh`.
- **Tests are gtest binaries run directly**, with `--gtest_filter`. There is no ctest registration.
  Tests that need game data **skip** when it is absent and **fail** when the path is set and wrong —
  a silent skip looks like a pass.
- **`openmw.cfg` already points at the Morrowind install**, so nothing needs `--data`. The harness
  runs from `build/`, since `--resources` defaults to `./resources`.

## Verification, after changing code and before saying it works

Build the targets you touched, run the test binary that covers them with a filter, then format.
Building the world to check a one-line change in the harness is waste; so is claiming a change works
because it compiled.

**Do not open the game window to check a rendering change.** `openmw-rtxtool shot` renders the real
renderer headlessly in about a second and prints a summary line — hit fraction, camera, frame time —
and it takes a camera on the command line, so a hypothesis about one frame can usually be settled
without a screenshot ever being looked at. `scene` answers "what was the renderer handed" without
drawing, and `bench` is the only headless path with a moving camera, so it is what reproduces
anything depending on motion or on cells arriving. `view` is for what only a window shows: how
something moves, whether an artefact is a still or a shimmer — and `--frames N` lets something that
cannot click drive it.

**Do not bench, and do not report frame times, until the renderer draws everything the game has** —
*Feature-complete first, then fast* taken to its conclusion. Land the feature, check it with `shot`,
and move on.

## Conventions

**C++20, `.clang-format` at 120 columns.** The user's global Rust rules do not apply to this tree;
the posture behind them does.

- **Comments say *why*.** A block comment on a type or a non-obvious function says what it is for.
  Inside a body, a comment earns its place by naming an invariant, a workaround and its cause, or a
  trade-off against the obvious alternative — never by restating the line under it. No decorative
  dividers.
- **Fix stale narration in code you are already editing**, like fixing indentation on a line you are
  changing. Sweeping files you are not otherwise in is a separate task.
- **Frame times are uniform, and an average that hides a spike is not an answer.** So work is made
  *incremental*, never *batched behind a threshold* — a table grows and recycles its slots rather
  than being compacted when enough of it has died, and a resource is appended rather than rebuilt
  when it changes. If an operation cannot be made cheap, it belongs off the frame path entirely, not
  on a rota. Report the p99 and the worst frame beside the median, because those are the ones a
  player feels.
- **Allocation is a metric on the frame path.** Persistent scratch buffers refilled with `clear()`,
  results into an out-parameter, nothing that constructs a `std::string` or a `std::function` per
  frame, logging that compiles out. There is a test that enforces this.
- **Asserts** guard contracts the code must keep, not data the world might supply. Hot paths use the
  debug-only form; untrusted input is never an assert.
