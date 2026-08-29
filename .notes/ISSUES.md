# Open issues

- `openmw-rtxtool verify`'s `island-crossing` view differs from itself run to run in one build
  (worst 4 of 255 on 0.01% of the pixels, release, upscaling off), while the other fifteen views
  are byte-identical between runs.

- Parts of a moving NPC stay behind in the air where the rest of the body has gone.

- Terrain still blinks on some frames of a moving camera: `bench --suite=streaming --albedo
  --exposure=1 --upscale=off` swings more than 40% on 26 frames of 600, where placing both frames
  into one table copy gives 5.

- `bench` reports a frame result for fewer than half its frames: `beginFrame`'s own drain consumes
  the oldest frame when the ring is full, so the `finishFrame` that follows finds nothing and the
  run's wait, GPU and hit rows are sampled from whichever frames it happened to reach.

