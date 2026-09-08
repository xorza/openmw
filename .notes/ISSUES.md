# Open issues

- `openmw-rtxtool doll --out=` writes the picture upside down. `TracedView` sets
  `RowOrder::BottomFirst` for the widget that shows it, and `StopWriter::writeDoll` reads the GUI
  texture straight into a PNG with no flip, where `writeView` flips row by row.

- `[RTX] filter` and `[RTX] jitter` reach nothing. `RtxRenderer::traceWorld` builds its
  `Rtx::FrameOptions` with `mSinceLast`, `mExposureBias` and `mExposure` only, so the backend takes
  the struct's own `mFilter = true` and `mJitter = false` whatever the profile says. The harness's
  `--filter` and `--jitter` are dead the same way — and `apps/rtxtool/repeatable.sh` passes
  `--filter=false`, so the determinism gate has been running with the wavelet on against a header
  that says the denoiser is off. `mExposure` carried the same defect and was fixed; these two were
  left with it.
