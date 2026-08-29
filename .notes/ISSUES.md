# Open issues


- The game's traced frame lights a room with the rasterizer's *adjusted* ambient. `RenderingManager::configureAmbient` (`apps/openmw/mwrender/renderingmanager.cpp:533-558`) scales an interior's `AMBI` ambient up to a relative luminance of `minimum interior brightness` — 0.08, in force because `classic falloff` defaults to false — before `mAmbientColour` reaches `apps/openmw/mwrender/rtx/rtxrenderer.cpp`, and `openmw-rtxtool` reads the record through `Rtx::makeRoomLight`. Berandas, Propylon Chamber writes 15/255: a played frame decodes the lifted 0.08 to 0.0072 linear and a `shot` decodes the record to 0.0048.
