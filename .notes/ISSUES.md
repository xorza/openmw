# Open issues

- `RtxVisibilityTest.aMoonTooFaintForItsOwnShadowStillLightsTheAir` fails on the Windows box
  (RTX 4090 Laptop, driver 616.92, Vulkan SDK 1.4.357.0), the same numbers every run: one leg
  delivers 0.3035 per unit of irradiance against a shadowed 0.0814, where the bound is twice.
  Every other RTX test passes there.
- `Rtx::dataPath` and `Rtx::featurePath` in `components/rtxvulkan/dlss.cpp` widen a `std::string`
  a byte at a time into the `wchar_t*` NGX takes. On Windows the wide string is UTF-16, so a
  temporary directory under a user name outside ASCII reaches NGX as the wrong characters.
- `rtx.sh <flavour> game` loads the quicksave from `$HOME/.local/share/openmw/saves/`, which is
  the Linux location; Windows keeps saves under `Documents/My Games/OpenMW/saves/`.
- `rtx.sh <flavour> gate` runs `clang-format-14`, which the Windows box has no route to, so the
  gate cannot run there.
