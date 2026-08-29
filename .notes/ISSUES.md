# Open issues


- `openmw-rtxtool` gives an interior no sun: `apps/rtxtool/cellscene.cpp` `loadRegion` returns `.mDaylight = {}` for a room ("its sun never shines"). The game lights a room with the cell's `AMBI` sunlight colour as a directional light from `interiorSunPos` (`apps/openmw/mwrender/renderingmanager.cpp:563-579`), and the game's RT path carries that sun — `apps/openmw/mwrender/rtx/rtxrenderer.cpp:790-797` reads `mSunPosition` and `mSunDiscColour.a()` of 1. A `shot` of a room and the played frame of the same room stand under different light.

- `openmw-rtxtool` places the light of every `LIGH` reference (`apps/rtxtool/cellscene.cpp`, the `SceneUtil::addLight` call in `readObjects`), including records flagged `OffDefault`. The game does not: `apps/openmw/mwclass/light.cpp:45` passes `allowLight = !(flags & OffDefault)`. `SceneUtil::LightCommon::mOffDefault` is read nowhere.
