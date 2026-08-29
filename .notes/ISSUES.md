# Open issues

- `openmw-rtxtool verify`'s `island-crossing` view differs from itself run to run in one build
  (worst 4 of 255 on 0.01% of the pixels, release, upscaling off), while the other fifteen views
  are byte-identical between runs.

- Parts of a moving NPC stay behind in the air where the rest of the body has gone.

- Terrain blinks hard in `view` at a standing camera — `-2,-9` at `-6087, -70048, 2978`,
  bearing 34°, climb 4°, day 0, 12:00, Clear. `bench` at the same spot, pipelined, is flat.

- Every cell crossing submits an empty frame: `bench --suite=streaming` reports nineteen crossings
  and nineteen frames whose primary hit count is exactly nought.

