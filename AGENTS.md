# AGENTS.md

## What this is

A fork of OpenMW 0.52 whose purpose is an **experimental ray-traced renderer**. Upstream OpenMW is
the host engine — cells, references, physics, scripts, animation, weather, GUI — and it keeps all of
that. What it stops owning is the picture.

This file records goals and working rules. Read `apps/openmw/mwrender/renderer.hpp` and its callers
before changing the renderer seam; follow resource and scene data back to their owners before
changing how the RT path consumes them. Anything the tree, `--help` or a commit already answers
does not belong here.

**`/home/xxorza/Projects/rtxmw/`** is the reference implementation: a Rust Morrowind ray tracer with
working water, caustics and volumetric fog. Its `docs/design.md` collects findings about
*Morrowind's content* — ray offsets on sheet geometry, Z-first Euler angles, the pre-lit albedo
problem — so read the relevant section before debugging something that looks already solved. Its
shaders are more current than its prose. Same author, MIT OR Apache-2.0; this fork is GPLv3.

## Posture

A 2002 game made to look astonishing on current hardware — ray-traced visibility, path-traced
indirect light, materials recovered from pre-lit vanilla textures, DLSS Ray Reconstruction, opacity
micromaps, SER. Vanilla content, new light transport.

Priorities, in order:

1. **How it looks.** Trading image quality for simplicity or convenience is the wrong trade.
2. **Performance.** 1920×1080 internal → 3840×2160 at 60 fps.

Nothing else ranks: no mod compatibility, no configurability for its own sake, no portability layer,
no abstraction over hardware this does not target.

**Feature-complete first, then fast.** An optimisation aimed at a frame that is about to change
shape needs its measurement taken again anyway. Land what is missing, note what it costs, act on the
number later. The exception is a cost so large it stops the work — a harness too slow to look at —
and that is a judgement to state out loud, not a licence.

Sports programming — strongest technique over safest, fast path first, delete what stopped earning
its place, settle arguments by measuring. Nothing here is published, so rewriting beats working
around.

## Against upstream

Upstream's constraints are not ours. Where they conflict, ours win. Where the two have to meet,
priorities in order:

1. **A clean seam, and one answer shared with the old renderer.** No hacks: the RT path asks its
   question of whatever holds the answer rather than reverse-engineering where the answer was put,
   and the two renderers read one description rather than each deriving its own. A callback chain
   walked for a type, a `dynamic_cast` standing in for a question, a second copy of a fact the game
   already states — each buys a smaller diff, and none is worth it.
2. **The smallest diff against upstream.** What the first does not settle is settled by what a
   reviewer has to read: fewer upstream files touched, fewer lines in each, an addition in
   preference to an edit.

**Two renderers in one binary, and the one not chosen never starts.** `-DOPENMW_RTX=ON` decides
whether the ray tracer is *built*; `[RTX] enabled` decides whether it *runs*, read once before the
window exists — not a refactor of the existing renderer, not a strategy pattern bolted onto
`RenderingManager`. With it on, **OpenGL is not initialized at all**: no GL context, no `osgViewer`
window, no interop, no rasterized frame underneath. The window is an SDL surface, the GUI is drawn
by the backend, and the inventory doll and the maps are traces. OSG stays, as a scene graph and a
content loader.

**The rasterizer's behaviour is never changed — a change to it is a bug, including one nobody can
see.** It is not modified, not wrapped and not conditionally compiled around; it is the path not
taken, which is what makes "does the RT path do this correctly" answerable by comparison.

**Upstream's files are read-only.** A change lands in the RTX-owned places and nowhere else:
`components/rtx*/`, `components/surface/`, `components/myguirtx/`, `apps/rtxtool/`,
`apps/openmw/mwrender/rtx/`, `apps/components_tests/{rtx,rtxbench,rtxtool,surface}/`, `files/rtx/`
and `.notes/`. Where the RT path cannot work without touching an upstream file, name the file and the
change and wait for a go-ahead. What is allowed is lifting shared code into `components/` so both
hosts read one answer — `components/sky/`, `components/weather/` and `components/sceneutil/vismask.hpp`
are that — with the rasterizer still reading what it read before. Git shows those lifts as a delete
and a create unless it is asked for `-M20%`. A gap in upstream's data is met by a hard failure
naming it, never by a patch to it.

**No merge-back discipline inside the RTX places.** This code is not upstreaming; do not shape it
around what a GitLab reviewer would accept.

**Read the old renderer first, every time.** Before fixing a bug or writing new code in the RT path,
find what `apps/openmw/mwrender/gl/` and the components under it already do about it. Morrowind's
own feel is the target: a number the game states beats one derived here, and a behaviour it has
beats one invented here. Most things that look like a gap are a field the RT path stopped
carrying.

**Rasterizer workarounds do not come across.** Render-bin ordering, the transparent pass, the
distortion pass, shadow-map tuning — the RT path answers those with rays. The line is what the
workaround is *for*: a fix for how a triangle got onto a screen stays behind, a decision about what
the world looks like comes over.

**A missing extension or feature is a hard failure naming it**, never a fallback path.

## Where the code lives

The picture is reached with **Vulkan on Ada-class NVIDIA**, behind an API-neutral core rather than
through a portability layer. The core stays neutral although one backend reads it: what belongs
there is what is true of the content and of light transport, and a fact about Vulkan that leaks into
it is a bug whether or not a second backend ever arrives.

- `components/rtx/` — the core: the scene description, the light transport, what the scene *is*.
  Written once, and it carries no graphics API and no game headers.
- `components/rtxvulkan/` — the backend, reached through `components/rtxbackends/`. What is true of
  an API lives here and nowhere else.
- `components/rtxbench/` — the instruments a measured run is taken with: what a run's length is
  written as, what a place came to, how it is printed and recorded, the card's clock, perf's fifo,
  a frame hash, a scene digest and a texture sheet. It knows nothing about a world.
- `components/myguirtx/` — MyGUI's backend. `components/surface/` — what the content says a surface
  is.
- `apps/openmw/mwrender/rtx/` — the game-side owner. `apps/rtxtool/` — the harness.
  `MWRender::Renderer` — the seam against `mwrender/gl/`.

## Traps

`build-debug/` is the everyday build, `openmw-rtxtool --help` lists the harness, and `CI/check_*.sh`
are the gates. What those do not tell you:

- **CMake's own `RelWithDebInfo` carries `-DNDEBUG`**, which compiles out every `assert` in the
  tree — so the contracts this code states everywhere are checked by nothing, in the build
  everybody develops in. Both debug directories override `CMAKE_{C,CXX}_FLAGS_RELWITHDEBINFO` to
  `-O2 -g` for that one reason, and `grep -c NDEBUG build-*/build.ninja` is how a directory says
  which kind it is. `GuiTextures` reached a commit reading write-combined memory through the
  accessor that refuses it, and nothing in a full test run could tell.
- **Three build directories, configured by the scripts in `apps/rtxtool/`.**
  `apps/rtxtool/debug.sh` makes `build-debug/`, the everyday one;
  `apps/rtxtool/debug-asan.sh` makes `build-debug-asan/` and runs the tests under it, `tool` in front
  of an argument sending it to the harness instead; `apps/rtxtool/release.sh` makes
  `build-release/`, which is `-O3 -DNDEBUG` and is where a number is taken. Each script states its
  configuration; the sanitizer script also sets and explains the required `ASAN_OPTIONS`.
- **`.refs/` is where a reference checkout goes, and nothing there is built.** NVIDIA's NGX SDK is
  750 MB of prebuilt binaries under NVIDIA's own licence, so it is named rather than vendored,
  submoduled or fetched — `extern/` is for source this tree compiles, and upstream keeps no
  submodules at all. `DLSS_SDK_DIR` in the environment points at it, one checkout serving every
  build directory here and `rtxmw` beside it. `components/rtxvulkan/CMakeLists.txt` states the
  clone command and the tag it is pinned to.
- **`bullet-dp`, not `bullet`**, if it ever has to be configured again: OpenMW needs a
  double-precision Bullet, the two Arch packages conflict, and the single-precision one has to come
  out first.
- **Never `cmake --build --clean-first`.** Upstream declares `files/lang/*.ts` — source-tree
  translation files, thousands of human translations — as byproducts of the `translations` target,
  so cleaning deletes them and the rebuild marks every translation `type="unfinished"`.
  `git checkout -- files/lang/` puts them back. Delete the build directory instead.
- **A pacman upgrade leaves stale objects that ninja cannot see.** Arch keeps each packaged file's
  own build mtime, so an upgraded header under `/usr/include` is usually *older* than the object
  that included the version before it — ninja compares mtimes, finds nothing to do, and links
  objects compiled against headers that no longer exist. gtest 1.17 to 1.18 did this to
  `build-debug-asan/`, and it surfaced as `mold: error: undefined symbol:
  testing::internal::GetWithoutMatchers()`; a symbol that stayed but changed meaning would have
  linked and run instead. `/var/log/pacman.log` says when the package landed, and
  `find <dir> -name '*.o' ! -newermt '<that time>' -delete` is the repair.
- **CI pins clang-format 14**; this box has 22 and they disagree, so run
  `CLANG_FORMAT=clang-format-14 CI/check_clang_format.sh`.
- **What a test can assert and what a check can are different questions.** `components-tests` holds
  what is true without a world: a spec, a record, a digest, a sheet. What is true *of* a world is
  `openmw-rtxtool check`, which asks it of a running game at every place of a suite and exits
  non-zero on the first failure.
- **Tests are gtest binaries run directly**, with `--gtest_filter`; there is no ctest registration.
  Tests that need game data **skip** when it is absent and **fail** when the path is set and wrong —
  a silent skip looks like a pass.
- **`openmw.cfg` already points at the Morrowind install**, so nothing needs `--data`. The harness
  runs from its own build directory, since `--resources` defaults to `./resources`.

## Verification, after changing code and before saying it works

Build the targets you touched, run the test binary that covers them with a filter, then format.
Building the world for a one-line change in the harness is waste; so is claiming a change works
because it compiled.

**Every verb drives a real game, headless.** `openmw-rtxtool` starts an engine, teleports to the
place a view names, warms the world up and then does whatever the verb asks — so the cells are read
by `MWWorld::Scene`, the people are dressed by `NpcAnimation` and the sky is reported by
`MWWorld::WeatherManager`. There is no second world any more, and no second bench.

**Do not open the game window to check a rendering change.** `shot` writes one frame with no window
and prints the hit fraction, the scene it was handed and the frame time — enough to settle most
hypotheses without looking at a picture. `scene` answers "what was the renderer handed" without
drawing. `bench` is what has a moving camera, so it reproduces anything depending on motion or on
cells arriving. `check` asserts what the tree claims about both. `view` is for what only a window
shows — how something moves, whether an artefact is a still or a shimmer — and it is the game, with
the player's own camera and collision off; `--frames N` closes it for something that cannot click.

**A run is the same run twice, and three clocks say so.** `[RTX] fixed step` is how far the
simulation steps and how long the renderer is told a frame took, and a stop's own frame count is
what the trace's sampler and the upscaler's jitter are walked by. A game's frame number carries
every frame a loading screen happened to draw, which is why it is not that.

**No benching and no frame times until the renderer draws everything the game has.** Land the
feature, check it with `shot`, and move on.

**Measure on a hot card, and never sleep between runs.** A cooldown is the wrong instrument: it
costs more wall time than every measurement it guards, and it starts each leg of an A/B from a
different thermal and clock state, which is the variance it was meant to remove. Warm the part
instead — one `bench` of the same views, thrown away — then run the legs back to back and
interleaved, so any drift left reaches both equally. The harness prints the core clock and the
temperature beside every result, and *that* is the check: a run whose clock or temperature differs
from its neighbour's is the run to repeat. **Buy confidence with repeats and not with waiting** — a
repeat of `--views=<one> --seconds=10` costs thirteen seconds where a cooldown costs a minute, so
six alternations still come in under two minutes. Half an hour of idling is never the answer to a
noisy number.

**Profiling.** `apps/rtxtool/profile.sh` records the CPU with `perf` over the measured frames only.
Nsight Systems is installed, and this machine's driver lets a non-root user profile the device —
`/etc/modprobe.d/nvidia-profiling.conf` sets `NVreg_RestrictProfilingToAdminUsers=0` — so a GPU
timeline is `nsys profile ./openmw-rtxtool bench ...` and needs no sudo. Nsight Compute (`ncu`) is
not installed, so what a pass costs *inside* the trace kernel is still measured by removing it and
re-running `shot`.

## Conventions

**C++20, `.clang-format` at 120 columns.** The user's global Rust rules do not apply to this tree;
the posture behind them does.

- **`#pragma once`, and includes in five blocks.** Every header in the RTX places opens with
  `#pragma once` rather than a named guard. `components/rtx/shaders/*.h` is the one exception, and
  `portable.h` says why: `glslc` warns that it is not implemented and carries on. Below the guard
  come the blocks a blank line apart, in this
  order: the file's own header, the C++ standard library, `<gtest/...>` where a test needs it, other
  libraries, `<components/...>` and `<apps/...>`, then quoted local headers. `.clang-format`
  preserves the blocks and sorts inside each, so the order is the author's and the sorting is not.
  A conditional `#include` goes last, after every unconditional one. A block out of order needs the
  comment saying why, the way `dlsspass.cpp` does for NGX.
- **Include what you name.** A file that spells `std::size_t` includes `<cstddef>`, whatever else
  happens to drag it in. A `.cpp` may lean on its own header for what that header's interface
  already needs, and on nothing else.
- **Comments say *why*.** A block comment on a type or a non-obvious function says what it is for.
  Inside a body, a comment earns its place by naming an invariant, a workaround and its cause, or a
  trade-off against the obvious alternative — never by restating the line under it. No decorative
  dividers.
- **Fix stale narration in code you are already editing**, like fixing indentation on a line you are
  changing. Sweeping files you are not otherwise in is a separate task.
- **Frame times are uniform**, and an average that hides a spike is not an answer. Work is
  *incremental*, never *batched behind a threshold*: a table recycles its slots rather than being
  compacted once enough of it has died, a resource is appended rather than rebuilt. What cannot be
  made cheap belongs off the frame path entirely, not on a rota. Report the p99 and the worst frame
  beside the median — those are the ones a player feels.
- **Allocation is a metric on the frame path.** Persistent scratch buffers refilled with `clear()`,
  results into an out-parameter, no `std::string` or `std::function` per frame, logging that
  compiles out. A test enforces it.
- **Asserts** guard contracts the code must keep, not data the world might supply. Hot paths use the
  debug-only form; untrusted input is never an assert.
