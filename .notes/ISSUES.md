# Open issues

- Ashstorm and Blight have no `Weather_Ashstorm_*` / `Weather_Blight_*` fallback entries in `build/openmw.cfg` or `~/.config/openmw/openmw.cfg`, so their land fog depth, wind speed and colours read as zero in both the game and `openmw-rtxtool`. `--weather Ashstorm` renders with no weather fog and no wind.
- `RtxVisibilityTest.aGlowJoinsTheLightAndAGlowingMapIsAddedPastIt` fails at HEAD: it expects a quarter glow to encode to 170 on the old scale of 1.6, and `EMISSIVE_INTENSITY` is 16 since "Add Direct Lighting For Emissive Surfaces", so every glow it measures clips to 255.
