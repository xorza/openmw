# Open issues

- `runInfo` in `apps/rtxtool/main.cpp` builds its `Rtx::RendererOptions` with a designated
  initializer list that skips `mCacheDirectory`, so every build of the harness warns
  `missing initializer for member 'Rtx::RendererOptions::mCacheDirectory'`.

- Standing inside the blight cloud at Dagoth Ur (cell `2,8` at 19247, 70446, 13757, bearing 128°,
  climb -27°, weather Blight) draws a large dark disc with a hard rim through an otherwise red
  frame. It is not the upscaler — `--upscale=off` shows it — and it is neither the sprite walk nor
  the medium walk: it survives `--props=0` and it survives the medium classification turned off. It
  is visible in the `--albedo` channel, so it is a hole in a translucent shell that covers the rest
  of the view.
