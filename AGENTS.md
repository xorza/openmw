# AGENTS.md

## What this is

A fork of OpenMW 0.52 whose purpose is an **experimental ray-traced renderer**. Upstream OpenMW
stays the host engine — cells, references, physics, scripts, animation, weather, GUI. It stops
owning the picture.

Read `apps/openmw/mwrender/renderer.hpp` and its callers before changing the renderer seam, and
follow scene or resource data back to its owner before changing how the RT path consumes it. What
the tree, `--help` or a commit already answers does not belong here.

## Posture

A 2002 game made to look astonishing on current hardware — ray-traced visibility, path-traced
indirect light, materials recovered from pre-lit vanilla textures, DLSS Ray Reconstruction. Vanilla
content, new light transport.

## Priorities:

- Avoid changing rasterizer and not related to Rtx renderer logic
- Rasterizer and Rtx renderers should have same interfaced and avoid exposing internal implementation details. Avoid branching depending on specific renderer implementation details - such cases should be abstracted.
- Performance is very important. Avoid repeating computations, precompute as early as possible.
- Target GPUs - Nvidia RTX 20XX and later.
- One binary ships two renderers, and the one not chosen never starts.
- Minimal diff with upstream, but without sacrificing code reusability and renderer abstraction quality. Rtx config page and translation changes in diff are fine.\

## Where the code lives

**Vulkan on ray-tracing NVIDIA hardware, Turing and later**, behind an API-neutral core rather than
a portability layer. A fact about Vulkan that leaks into the core is a bug whether or not a second
backend ever arrives.

- `components/rtx/` — the core: the scene description, the light transport, what the scene _is_. No
  graphics API, no game headers.
- `components/rtxvulkan/` — the backend. What is true of an API lives here and nowhere else; the
  two places that stand one up name `VulkanRenderer` and nothing else does.
- `components/rtxbench/` — the instruments a measured run is taken with: a run's length, what a
  place came to, how it is printed and recorded, the card's clock, perf's fifo, a frame hash, a
  scene digest and a texture sheet. It knows nothing about a world.
- `components/myguirtx/` — MyGUI's backend.
- `apps/openmw/mwrender/rtx/` — the game-side owner. `apps/rtxtool/` — the harness.
  `MWRender::Renderer` — the seam, and `GlRenderer` beside upstream's files in `mwrender/`.

## Verification

- Build the targets you touched, run the covering test binary with a filter, then format.
  Compiling is not verifying.
- `components-tests --gtest_filter='Rtx*'` and `openmw-tests --gtest_filter='Rtx*'` once before
  saying it works. `rtx debug gate` once at the end: format check, build, no-assert and no-DLSS
  compiles, tests, `check`, one repeat pair. Never a gate beside a build or another gate.
- Do not open the game window to check a rendering change. `shot --views=all --map
  --against=<dir>` says which pictures a change moved. `scene` reports what the renderer was
  handed. `check` asserts the tree's claims at every place of its suite. `bench` has the moving
  camera. `view` is for what only a window shows.
- `rtx debug repeat --pairs=10` after touching anything a frame reads. A run is the same run
  twice, and a pair that finds nothing has found nothing. Read a difference with `--exposure=1`
  and `--pictures=<dir>`.
- Measure on a hot card, back to back, never with a sleep between runs. Take a throwaway
  warm-up leg first. No frame times until the renderer draws everything the game has.
- Profiling: `apps/rtxtool/profile.sh` for the CPU, `nsys profile ./openmw-rtxtool bench ...`
  for the GPU. `ncu` is not installed.

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
- **Comments say _why_**: an invariant, a workaround and its cause, a trade-off against the obvious
  alternative — never a restatement of the line under it. No decorative dividers.
- **Fix stale narration in code you are already editing.** Sweeping files you are not otherwise in
  is a separate task.
- **Frame times are uniform**, and an average that hides a spike is not an answer. Work is
  _incremental_, never _batched behind a threshold_: a table recycles its slots, a resource is
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
