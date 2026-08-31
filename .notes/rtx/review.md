# Review of the RTX fork against `upstream/master`

Merge base `d720b4c82ccfa1f4d89353699929545945d79a2c`. The diff is 631 files, +94114/−4730. The
upstream-owned places (the moved `mwrender/gl/` files included) hold 229 files, +12110/−4730; the
rest is RTX-owned.

Delete an item when it is addressed. Delete a heading when it is empty. This file lists open items
only. The two frame-cost items moved to `performance.md` §3 lane D, which is where frame work is
tracked now.

Each item describes what is there and what it costs. No item says how to fix it.

## Upstream behaviour changed further than the seam needed

- [ ] The game's own crash box is gone for anyone who launched from a terminal. Upstream's
      crashcatcher showed the message box unconditionally; `Debug::wantsFatalDialog()` now
      suppresses it whenever stdout or stderr is a tty, and that test runs in the game as well as
      the tools (`components/crashcatcher/crashcatcher.cpp:432`,
      `components/debug/debuglog.cpp`). The only caller of `setFatalDialogs(false)` is
      `apps/rtxtool/main.cpp:841`, so the tool half of the change did not need the tty half.

## A comment says one thing and the code under it another

- [ ] The `RtxRenderer::freezeFrame` declaration says the method hands back "one black texel"
      on purpose ("Flat black, and the loading screen puts it up as the backdrop",
      `apps/openmw/mwrender/rtx/rtxrenderer.hpp`). The body reads the presented frame back and
      shows it, and black only covers the very first load before anything was presented
      (`rtxrenderer.cpp:503`). The declaration is the stale half.

## One fact, stated twice

- [ ] The `WorldState` field rationales are written out twice. `mSunDiscColour` ("the other sun
      colour"), `mCloudBlend` ("the deck's own crossing") and the sun position/vector story each
      carry a full essay in `apps/openmw/mwrender/renderingmanager.hpp` and a second full essay in
      `apps/openmw/mwrender/sceneframe.hpp`. Two copies of one explanation drift apart, and one of
      the copies sits in an upstream-owned file and is diff against upstream.

- [ ] Two bilinear samplers landed in one change: `MyGUIPlatform::sampleBilinear`
      (`components/myguiplatform/pixels.cpp`) and the file-local `resample` in
      `apps/openmw/mwrender/globalmap.cpp`. Both implement GL_LINEAR-with-clamp filtering over
      packed RGBA8 and they round differently (`+ 0.5f` against `std::lround`).

- [ ] `EsmLoader::ModelRecords` says "adding a type is this line alone", but `Query`'s per-type
      flags and the four `if constexpr` branches of `wanted<T>()` in `load.cpp` are a second list
      that has to agree with the tuple by hand, and nothing checks that the two agree.

## A setting that silently does nothing

- [ ] `RtxRenderer::setVSync` is an empty override while the settings window still offers "VSync
      mode", so under the ray tracer the change is accepted and nothing happens. The fork's own
      standard, written beside the ray tracing switch in `settingswindow.cpp`, is that a dead
      control says why it is dead.

## The upstream diff carries lines that do nothing

- [ ] `apps/openmw/mwrender/renderingmanager.hpp` forward-declares
      `namespace Fx { class StateUpdater; }` and nothing in the header names `Fx::`.

- [ ] Fourteen upstream files, about 68 diff lines, differ from upstream only by the
      `MWRender::Mask_*` → `SceneUtil::Mask_*` spelling:
      `mwclass/{activator,door,esm4base,static}.cpp`, `mwmechanics/actors.cpp`,
      `mwrender/{actorspaths,bulletdebugdraw,effectmanager,groundcover,navmesh,objects,pathgrid,recastmesh}.cpp`
      and `mwworld/projectilemanager.cpp`. This is the rename cost of the sanctioned vismask lift;
      every future upstream merge conflicts on those lines, and the fork's priorities (one
      canonical name over the smallest diff) are what decide whether it stays.

- [ ] `#include <components/sceneutil/vismask.hpp>` sits inside the quoted-include group — after
      `"util.hpp"` and its siblings — in `renderingmanager.cpp`, `localmap.cpp`,
      `characterpreview.cpp`, `worldimp.cpp` and the `mwclass` files, against the include grouping
      those files keep everywhere else.

- [ ] `components/esmloader/load.cpp` rewords upstream's log lines ("Prepared … unique cells" →
      "Merged across content files to … cells") beyond what the record-type generalisation
      required.

- [ ] An Apple bundling fix unrelated to the renderer (`if (APPLE AND USE_QT)`) rides in the root
      `CMakeLists.txt` diff.

## Asymmetries

- [ ] In `loadEsmData` every store is gated on its query flag, and `prepareLandTextures` alone
      runs unguarded (`load.cpp`). The result is empty when nothing was loaded, but it is the one
      ungated line in two otherwise symmetric blocks.
