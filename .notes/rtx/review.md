# Review of the RTX fork against `upstream/master`

Merge base `97c3f81abae4350d7265005ef37249154ac5a3b9`. The diff is 656 files, +96038/−4734. The
upstream-owned places (the moved `mwrender/gl/` files included) hold 230 files, +12197/−4734; the
rest is RTX-owned.

Delete an item when it is addressed or refused. Delete a heading when it is empty. This file lists
open items only.

Each item describes what is there and what it costs. No item says how to fix it.

## The upstream diff carries lines that do nothing

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
