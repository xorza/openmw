# Open issues

- `bench` measures under whatever the desktop draws, and every figure of the report moves with
  it, not only the tail. A run started from Claude Code's foreground runs under its spinner: Zed
  redraws the terminal for it and KWin composites the redraw, about nine times a second, and each
  redraw preempts the card for 3 to 10 ms. At the ship, 4K balanced: the frame median reads 15.6-15.8
  against 14.9-15.0 on an idle desktop, the p99 20.5-23.6 against 16.3, the 1% low 42-49 fps
  against 61; the zone shares read a tenth high (`upscale` 7.4-7.5 against 6.8, `trace` 5.7-5.8
  against 5.2); and the host rows a half to two thirds high (`walk` 1.9-2.0 against 1.1-1.2,
  `update` 1.6-1.7 against 1.2). The report has no line saying whether the card was shared.
  `nvmlDeviceGetProcessUtilization` — what `nvidia-smi pmon` reads — is one process every 200 ms,
  whoever held the card at the instant, so it catches a 4% stolen share in about one sample of
  twenty and cannot name a frame. `VK_KHR_global_priority` is no way round: the driver offers
  every family one priority, medium, and refuses `HIGH` and `REALTIME` with
  `VK_ERROR_NOT_PERMITTED_KHR`. `.notes/bench.txt` 2026-09-20 has the readings.
