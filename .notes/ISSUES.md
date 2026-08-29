# Open issues

- `openmw-rtxtool verify`'s `island-crossing` view differs from itself run to run in one build
  (worst 4 of 255 on 0.01% of the pixels, release, upscaling off), while the other fifteen views
  are byte-identical between runs.

- Parts of a moving NPC stay behind in the air where the rest of the body has gone.

- Every cell crossing submits an empty frame: `bench --suite=streaming` reports nineteen crossings
  and nineteen frames whose primary hit count is exactly nought.

- No caller ever has two frames in flight. `bench --suite=streaming` waits a median of 4.46 ms on a
  GPU frame of about 4.3 ms, so the CPU stands still for the whole of it.
