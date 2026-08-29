# Open issues


- `components/surface/material.hpp` documents `mTwoSided` as "true unless something says otherwise, because OpenGL culls nothing until it is told to", and defaults it to true. The game's scene root enables `GL_CULL_FACE` (`apps/openmw/mwrender/renderingmanager.cpp:369`), so a surface with no `NiStencilProperty` is single-sided in the rasterizer, and the three vanilla archives contain no `NiStencilProperty` at all. The content doubles every leaf, grass and cloth card as a reversed-winding copy instead (3670 shapes in 747 files). The description says the opposite of what the game draws.

- `CLANG_FORMAT=<clang-format 14> CI/check_clang_format.sh` fails on 17 files that are already committed, none of them in the current change: `apps/components_tests/rtx/{lightbuilder,texturebuilder,scenedesc,wavefield,visibilitypass}.cpp`, `apps/openmw/mwgui/windowmanagerimp.cpp`, `apps/openmw/mwrender/gl/glrenderer.cpp`, `apps/rtxtool/lighting.cpp`, `components/rtx/{offscreentrace,lightbuilder,texturebuilder}.cpp`, `components/rtx/{lightbuilder,renderer}.hpp`, `components/rtxvulkan/{exposurepass,compositepass}.cpp`, `components/sky/moonmodel.cpp`, `components/weather/precipitation.cpp`. Mostly a doubled blank line or an indentation the formatter disagrees with.

- `openmw-rtxtool shot --cell="Berandas, Propylon Chamber"` renders black apart from the portal's own red glow, at the measured exposure and at `--exposure 1` alike, and `Falasmaryon, Propylon Chamber` renders the same. `--albedo` shows the whole chamber, so the geometry arrives and the light does not.
