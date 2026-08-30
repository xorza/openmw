# Review of the RTX fork against `upstream/master`

Merge base `d720b4c82ccfa1f4d89353699929545945d79a2c`. The diff is 615 files, +92145/−4906.
The RTX-owned places hold about 385 files and 79,900 lines. The upstream-owned places hold 216
files, +10257/−4844.

Delete an item when it is addressed. Delete a heading when it is empty. This file lists open items
only.

Each item describes what is there and what it costs. No item says how to fix it.

## Work is done per frame that the frame did not change

- [ ] `components/rtxvulkan/guitextures.*` is synchronous. `getView`, `read` and `writeWith` all
      call `flush()`, which is a submit and a wait. `drawGui` calls `getView` per batch. A texture
      written in a frame costs one submit-and-wait inside that frame. `videowidget.cpp` writes one
      texture per frame.

## Two paths for one job, and asymmetric interfaces

- [ ] `components/rtxvulkan/shaders/lib/bindings.glsl` declares set-0 bindings out of order
      (15 before 14) with set-2 bindings `0, 6, 5, 1, 2, 4, 3, 8, 9, 7, 10` interleaved among them.
      `tone.comp` declares binding 3 before binding 2. `visibilitypass.cpp` `sBindings` builds the
      layout by index, so the reader has no one list to compare.
- [ ] `components/rtxvulkan/shaders/lib/lights.glsl:237-254` places `litCosine`'s doc inside
      `weighLamps`'s doc, and `weighLamps`'s `@param` block above `considerLamp`.
- [ ] `components/rtx/CMakeLists.txt` lists `compositequeue` after `spritetiles`.
      `components/rtxvulkan/CMakeLists.txt` lists `slottable.hpp` between `frameslots` and
      `gbuffer`. `apps/rtxtool/CMakeLists.txt` lists `lighting`, `motion`, `npc`, `objectstorage`,
      `posedactors`, `options`, `perfcontrol`, `picture`, `view`, `viewpoint`, `validationchoice`,
      `verify` out of alphabetical order.

## Harness command plumbing is copied per command

- [ ] `apps/rtxtool/main.cpp` `dispatch` (475-770) is one 300-line function of
      `variables["..."].as<...>()` copies, one block per command.
- [ ] `apps/rtxtool/bench.cpp` `runBench` (169-523) mixes timing, hashing, JSON, crossing
      accounting and reporting in one function. `report` and `writeJson` restate the same fields
      by hand in two formats.
- [ ] `apps/rtxtool/view.cpp` `runWindow` (116-511) holds a 100-line event lambda and the frame
      loop in one function.
- [ ] `apps/rtxtool/options.cpp:87-91` `--npc` help says "They come naked" while `--clothes`
      (line 101) defaults to on and dresses them.
- [ ] `files/rtx/views.cfg` `seyda-neen-customs` has a comment between two of its fields.

## Comments that describe code that no longer exists

- [ ] `apps/openmw/mwrender/rtx/rtxrenderer.hpp` `mComplained` carries the doc of another member
      ("The one sequence every mirror walk in this renderer poses at.").
- [ ] `components/rtxvulkan/vulkanrenderer.hpp:437-443` `mNgx` has two merged doc paragraphs. The
      first says `describeDevice` takes a share of its own; `describeDevice` now calls
      `Dlss::probe`.
- [ ] `components/rtxvulkan/vulkanrenderer.hpp:407-419` the view-chain doc ("Its own chain and not
      the frame's ... Grown to the largest picture asked for") sits on `mViewScenes`.
- [ ] `components/rtxvulkan/structurestorage.hpp:40-42` says the renderer is synchronous and a
      fence-keyed retirement list has to grow here. `Graveyard` exists and `release` buries rooms
      in it.
- [ ] `components/rtxvulkan/commands.hpp:17-20` says `CommandPool` is "Setup only" and nothing in
      it is shaped for a frame. `allocate`, `begin` and `submit` are the frame path.
- [ ] `components/rtxvulkan/swapchain.cpp:29-30` says tone mapping "is M8's, and this is where they
      will land". `TonePass` exists.
- [ ] `components/rtxvulkan/gbuffer.cpp:67-69` says the mask is "One float ... See `gbuffer.h` for
      why it is not a byte". `gbuffer.h` defines `GBUFFER_MASK` as `R8_UNORM` and argues that it is
      a byte. `readChannel` widens bytes.
- [ ] `components/rtxvulkan/physicaldevice.cpp:66-68` "Why a candidate was rejected, or empty when
      it was not." sits above `hasResizableBar`.
- [ ] `components/rtxvulkan/texture.hpp:67` says "the shader is told the count and does not index
      past it". No count reaches the shader.
- [ ] `apps/rtxtool/cellscene.hpp:59-73` glues two doc blocks on `LoadedCell`, and 137-150 glues
      two on `RegionLoad`. `apps/rtxtool/stagedworld.hpp:112-119` puts `driveWeather`'s doc on
      `frame`.
- [ ] `apps/rtxtool/main.cpp:490-492` states "Boost skips the first token" twice.
- [ ] `components/rtxvulkan/shaders/lib/sprites.glsl:17-27` splits `puffLight`'s doc with an empty
      `///` line. `bindings.glsl:258-259` has a double blank line.
