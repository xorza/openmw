# Open issues

- `GuiTextures::drop` puts a texture in `mRetired` and the next `flush` destroys it, having waited
  only for its own batch. `drawGui` submits with `mGuiFence` and waits for it two frames later, so
  the GUI submit that sampled the texture may still be running. `drop`'s comment says "every submit
  here waits, and so does the one that drew with it".
