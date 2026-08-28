# Open issues

- Ashstorm and Blight have no `Weather_Ashstorm_*` / `Weather_Blight_*` fallback entries in `build/openmw.cfg` or `~/.config/openmw/openmw.cfg`, so their land fog depth, wind speed and colours read as zero in both the game and `openmw-rtxtool`. `--weather Ashstorm` renders with no weather fog and no wind.
- `spritesAlong` in `components/rtxvulkan/shaders/lib/sprites.glsl` drops a sprite whose centre lies past the surface hit (`depth >= limit`) and draws one in front of it in full, so a puff that straddles a wall or the ground clips hard along the surface and pops in and out as its centre crosses it.
