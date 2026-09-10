# AGENTS.md

## What this is

A fork of OpenMW 0.52 whose purpose is an **experimental ray-traced renderer**. Upstream OpenMW
stays the host engine — cells, references, physics, scripts, animation, weather, GUI. It stops
owning the picture.

Read `apps/openmw/mwrender/renderer.hpp` and its callers before changing the renderer seam, and
follow scene or resource data back to its owner before changing how the RT path consumes it. What
the tree, `--help` or a commit already answers does not belong here.

**`/home/xxorza/Projects/rtxmw/`** is the reference implementation: a Rust Morrowind ray tracer with
working water, caustics and volumetric fog. Its `docs/design.md` collects findings about
*Morrowind's content* — ray offsets on sheet geometry, Z-first Euler angles, the pre-lit albedo
problem — so read it before debugging something that looks already solved. Its shaders are more
current than its prose. Same author, MIT OR Apache-2.0; this fork is GPLv3.

## Posture

A 2002 game made to look astonishing on current hardware — ray-traced visibility, path-traced
indirect light, materials recovered from pre-lit vanilla textures, DLSS Ray Reconstruction. Vanilla
content, new light transport.

Priorities, in order:

1. **How it looks.** Image quality is not traded for simplicity or convenience.
2. **Performance.** 1920×1080 internal → 3840×2160 at 60 fps.

Nothing else ranks: no mod compatibility, no configurability for its own sake, no portability layer,
no abstraction over hardware this does not target.

**Turing and later — every card with hardware ray tracing.** What the frame path asks for exists on
Turing, and a card below that floor is a hard failure naming what it lacks. Anything newer is an
optional accelerator decided once at device creation, and a second path per architecture needs a
measurement saying the gain is real — Ada's reordering and its opacity micromaps both failed that
measurement here and are gone.

**Feature-complete first, then fast.** Land what is missing, note what it costs, act on the number
later. A cost large enough to stop the work is the exception, and it is said out loud.

Strongest technique over safest, delete what stopped earning its place, settle arguments by
measuring. Nothing here is published, so rewriting beats working around.

## Against upstream

Where upstream's constraints conflict with ours, ours win. Where the two have to meet:

1. **A clean seam, and one answer shared with the old renderer.** The RT path asks its question of
   whatever holds the answer instead of reverse-engineering where the answer was put, and both
   renderers read one description. A callback chain walked for a type, a `dynamic_cast` standing in
   for a question, a second copy of a fact the game already states — each buys a smaller diff, and
   none is worth it.
2. **The smallest diff against upstream.** Fewer files touched, fewer lines in each, an addition in
   preference to an edit.

**Two renderers in one binary, and the one not chosen never starts.** `-DOPENMW_RTX=ON` decides
whether the ray tracer is built; `[RTX] enabled` decides whether it runs, read once before the
window exists. With it on, **OpenGL is not initialized at all**: no GL context, no `osgViewer`
window, no interop, no rasterized frame underneath. The window is an SDL surface, the GUI is drawn
by the backend, and the inventory doll and the maps are traces. OSG stays, as a scene graph and a
content loader.

**The rasterizer's behaviour is never changed** — not modified, not wrapped, not conditionally
compiled around. It is the path not taken, which is what makes "does the RT path do this correctly"
answerable by comparison. Its workarounds do not come across either: render-bin ordering, the
transparent pass, the distortion pass and shadow-map tuning are answered with rays. A fix for how a
triangle got onto a screen stays behind; a decision about what the world looks like comes over.

**Upstream's files are read-only.** Changes land in `components/rtx*/`, `components/surface/`,
`components/myguirtx/`, `apps/rtxtool/`, `apps/openmw/mwrender/rtx/`,
`apps/components_tests/{rtx,rtxbench,rtxtool,surface}/`, `files/rtx/` and `.notes/`, and nowhere
else. Where the RT path cannot work without touching an upstream file, name the file and the change
and wait for a go-ahead. Lifting shared code into `components/` so both hosts read one answer is
allowed — `components/sky/`, `components/weather/` and `components/sceneutil/vismask.hpp` are that —
with the rasterizer still reading what it read before. Git shows a lift as a delete and a create
unless it is asked for `-M20%`.

**A gap in upstream's data, a missing extension or a missing feature is a hard failure naming it** —
never a patch to upstream, never a fallback path.

**No merge-back discipline inside the RTX places.** This code is not upstreaming.

**Read the old renderer first, every time.** Find what `apps/openmw/mwrender/gl/` and the components
under it already do about it. A number the game states beats one derived here, and a behaviour it
has beats one invented here. Most apparent gaps are a field the RT path stopped carrying.

## Where the code lives

**Vulkan on ray-tracing NVIDIA hardware, Turing and later**, behind an API-neutral core rather than
a portability layer. A fact about Vulkan that leaks into the core is a bug whether or not a second
backend ever arrives.

- `components/rtx/` — the core: the scene description, the light transport, what the scene *is*. No
  graphics API, no game headers.
- `components/rtxvulkan/` — the backend, reached through `components/rtxbackends/`. What is true of
  an API lives here and nowhere else.
- `components/rtxbench/` — the instruments a measured run is taken with: a run's length, what a
  place came to, how it is printed and recorded, the card's clock, perf's fifo, a frame hash, a
  scene digest and a texture sheet. It knows nothing about a world.
- `components/myguirtx/` — MyGUI's backend. `components/surface/` — what the content says a surface
  is.
- `apps/openmw/mwrender/rtx/` — the game-side owner. `apps/rtxtool/` — the harness.
  `MWRender::Renderer` — the seam against `mwrender/gl/`.

## Traps

`build-debug/` is the everyday build, `openmw-rtxtool --help` lists the harness, and `CI/check_*.sh`
are the gates. What those do not tell you:

- **CMake's own `RelWithDebInfo` carries `-DNDEBUG`** and compiles out every `assert` in the tree.
  Both debug directories override `CMAKE_{C,CXX}_FLAGS_RELWITHDEBINFO` to `-O2 -g` for that one
  reason, and `grep -c NDEBUG build-*/build.ninja` says which kind a directory is.
- **Three build directories, configured by the scripts in `apps/rtxtool/`.** `debug.sh` makes
  `build-debug/`. `debug-asan.sh` makes `build-debug-asan/` and runs the tests under it — `tool` in
  front of an argument sends it to the harness instead — and it sets and explains the required
  `ASAN_OPTIONS`. `release.sh` makes `build-release/`, which is `-O3 -DNDEBUG` and is where a number
  is taken.
- **`.refs/` is where a reference checkout goes, and nothing there is built.** NVIDIA's NGX SDK is
  750 MB of prebuilt binaries under NVIDIA's own licence, so it is named rather than vendored. The
  build scripts want `OPENMW_DLSS_SDK` in the environment and refuse without it; CMake on its own
  falls back to `DLSS_SDK_DIR`, then to `.refs/dlss`. This box points at `rtxmw`'s checkout, so this
  tree has no `.refs/` of its own. `components/rtxvulkan/CMakeLists.txt` states the clone command
  and the pinned tag.
- **`bullet-dp`, not `bullet`** — OpenMW needs a double-precision Bullet, the two Arch packages
  conflict, and the single-precision one has to come out first.
- **A pacman upgrade leaves stale objects that ninja cannot see.** An upgraded header under
  `/usr/include` is usually *older* than the object that included the previous version, so ninja
  finds nothing to do and links objects compiled against headers that no longer exist. It surfaces
  as an undefined symbol at link, or as nothing at all when a symbol stayed and changed meaning.
  `/var/log/pacman.log` says when the package landed, and the repair is
  `find <dir> -name '*.o' ! -newermt '<that time>' -delete`.
- **CI pins clang-format 14**; this box has 22 and they disagree, so run
  `CLANG_FORMAT=clang-format-14 CI/check_clang_format.sh`.
- **`components-tests` holds what is true without a world** — a spec, a record, a digest, a sheet;
  `openmw-tests` holds what needs the game's own types. What is true *of* a world is
  `openmw-rtxtool check`, which asks it of a running game at every place of a suite and exits
  non-zero on the first failure.
- **Tests are gtest binaries run directly**, with `--gtest_filter`; there is no ctest registration.
  Tests that need game data **skip** when it is absent and **fail** when the path is set and wrong —
  a silent skip looks like a pass.
- **`openmw.cfg` already points at the Morrowind install**, so nothing needs `--data`. The harness
  runs from its own build directory, since `--resources` defaults to `./resources`.

## Verification, after changing code and before saying it works

Build the targets you touched, run the test binary that covers them with a filter, then format.
Building the world for a one-line change in the harness is waste, and so is calling a change
verified because it compiled.

**What each step costs on this box**, because the rule above is only worth keeping if the numbers
are known: `ninja` with nothing to do 0 s; a rebuild after touching a header 84 objects read 1 s;
`components-tests --gtest_filter='Rtx*'` 16 s, of which 7 is four upscaler tests; the same filtered
to `Rtx*Cell*` 2 s; one `bench` place 20 s; `check` 122 s; `repeatable.sh --pairs=1` 70 s.

**The build is not the slow part.** `ccache` and `mold` are configured and the cache runs about
seventy per cent hits. Filter the tests to what the change touched and run the whole `Rtx*` once,
before saying it works — not after every edit. `repeatable.sh --pairs=2` while iterating and
`--pairs=10` once at the end.

**Never run a gate beside a build, or beside another gate.** The reading is then about the machine.
Take the throwaway warm-up leg before any A/B: a first `bench` after a gap read 2.29 ms against a
settled 1.62 to 1.67 at `seyda-neen-ship`.

**Every verb but `info` drives a real game.** `openmw-rtxtool` starts an engine, teleports to the
place a view names and warms the world up, so cells are read by `MWWorld::Scene`, people are dressed
by `NpcAnimation` and the sky is reported by `MWWorld::WeatherManager`. `info` reports the device
and stages no world. `shot` and `scene` open no window, and `bench` opens one unless
`--window=false`.

**Do not open the game window to check a rendering change.** `shot` writes one frame with no window
and prints the hit fraction, the scene it was handed and the frame time. `scene` answers what the
renderer was handed without drawing. `bench` has the moving camera, so it reproduces anything
depending on motion or on cells arriving. `check` asserts what the tree claims about both. `view` is
for what only a window shows — how something moves, whether an artefact is a still or a shimmer —
and it is the game, with the player's own camera and collision off; `--frames N` closes it.

**A run is the same run twice.** `[RTX] fixed step` is how far the simulation steps and how long the
renderer is told a frame took, and a stop's own frame count is what the trace's sampler and the
upscaler's jitter are walked by. A game's frame number counts loading-screen frames, which is why it
is not that.

**`apps/rtxtool/repeatable.sh` is what says it still is.** It walks `one-cell-walk` twice in two
processes and compares the frame hashes. Two processes, because two walks in one share no world
state and agree on nothing. Run it after touching anything a frame reads.

**The scene columns are the gate and the picture is a report.** What a run is handed — every column
of the hashes table — repeats exactly, and that is what a regression has to keep. The picture is
printed beside it and does not fail the run, because it carried a residual nobody hunted and a gate
red for that is a gate nobody reads. It is exact at the moment, over the pairs `repeatable.sh` has
been run for — enough of them and the picture earns the gate as well.

**A count of differing pictures says when, and never how much.** The exposure is measured off the
frame and approaches its target from the value it held, so every pixel depends on the whole frame and
every frame depends on the one before it. One frame the trace drew differently therefore moves every
frame after it, by the one part in 255 an eight-bit hash can barely hold — and the count is how early
that single event landed rather than how much went wrong. So a count is not a property of the
renderer: it scales with the walk. This file carried "26 or 27 frames of 45" taken over 45 frames,
while `repeatable.sh` has walked 360 since the day it was written — so the number never described
what the gate reports, and six pairs of the walk it does run differed on 204 to 343 pictures.

**`repeatable.sh` prints the current reading on every run**, and a number in prose here would go
stale the next time the walk changed length.

**A determinism reading needs ten pairs.** A pair that finds nothing has found nothing, and every
cause this fork has withdrawn was named from a single pair. `--pairs` is what runs them.

**And read a difference with `--exposure=1`.** A measured exposure couples every pixel of a frame to
every other and every frame to the one before it, so it is the one term that turns a single
divergence into a whole run of them. Held, the count comes nearer the frames that actually differ —
nearer and not exact, because the fog volume reprojects too. And read `Rtx::Channel::Radiance` as
floats, which carries what no eight-bit hash can show.

**No benching and no frame times until the renderer draws everything the game has.**

**Measure on a hot card, and never sleep between runs.** A cooldown costs more than the measurements
it guards, and it starts each A/B leg from a different clock state. Warm with one thrown-away
`bench` of the same views, then run the legs back to back and interleaved. The harness prints the
core clock and the temperature beside every result: a run whose clock or temperature differs from
its neighbour's is the run to repeat. Buy confidence with repeats, not with waiting: a repeat of
`--views=<one>` costs twenty-three seconds at the default twenty, so six alternations come in under
three minutes.

**Profiling.** `apps/rtxtool/profile.sh` records the CPU with `perf` over the measured frames only.
A GPU timeline is `nsys profile ./openmw-rtxtool bench ...`, and this driver needs no sudo for it.
`ncu` is not installed, so what a pass costs inside the trace kernel is measured by removing it and
re-running `shot`.

## Conventions

**C++20, `.clang-format` at 120 columns.** The user's global Rust rules do not apply to this tree;
the posture behind them does.

- **`#pragma once`, and includes in five blocks** a blank line apart: the file's own header, the C++
  standard library, `<gtest/...>`, other libraries, `<components/...>` and `<apps/...>`, then quoted
  local headers. `.clang-format` preserves the blocks and sorts inside each, so the order is the
  author's and the sorting is not. A conditional `#include` goes last, and a block out of order
  carries the comment saying why, the way `dlsspass.cpp` does for NGX. `components/rtx/shaders/*.h`
  is the one exception to `#pragma once`, and `portable.h` says why.
- **Include what you name.** A file that spells `std::size_t` includes `<cstddef>`. A `.cpp` may
  lean on its own header for what that header's interface already needs, and on nothing else.
- **Comments say *why***: an invariant, a workaround and its cause, a trade-off against the obvious
  alternative — never a restatement of the line under it. No decorative dividers.
- **Fix stale narration in code you are already editing.** Sweeping files you are not otherwise in
  is a separate task.
- **Frame times are uniform**, and an average that hides a spike is not an answer. Work is
  *incremental*, never *batched behind a threshold*: a table recycles its slots, a resource is
  appended rather than rebuilt. What cannot be made cheap belongs off the frame path entirely, not
  on a rota. Report the p99 and the worst frame beside the median.
- **Allocation is a metric on the frame path.** Persistent scratch buffers refilled with `clear()`,
  results into an out-parameter, no `std::string` or `std::function` per frame, logging that
  compiles out. A test enforces it.
- **Loading allocates no more freely than a frame does.** A loader is a persistent object owning its
  buffers, `clear()`ed and refilled for each thing it reads. Cells arrive while the game is running,
  so a spike taken at load is a spike a player feels.
- **Whatever can be computed once is computed once** — at initialization or at load. A frame reads
  what it was handed.
- **One path through a shader.** A single computation covering every case beats a tree that skips
  work per lane: a factor of zero, a table lookup, a value selected without a jump. Divergence needs
  a measurement saying the branch pays for itself, named where the branch lands.
- **Asserts** guard contracts the code must keep, not data the world might supply. Hot paths use the
  debug-only form; untrusted input is never an assert.
