# Open issues
- `rtxtool view --view=<name>` faces north: `Session::beginStop` writes the view's `pos` into the player's position and never its `look` into the rotation, so a window on `seyda-neen-ship` looks up the coast instead of at the town, and the stand it prints at the end reads bearing 0°.
