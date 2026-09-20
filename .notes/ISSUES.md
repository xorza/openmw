# Open issues

- `bench` measures under the desktop. `kwin_wayland` and an editor redraw at about nine hertz while
  a run goes and preempt the card for 1.3 to 2.8 ms each time (`nvidia-smi pmon` shows them at up
  to 19% SM during a run), so the p99, the worst frame and the 1% low of every place are the
  desktop's and not the renderer's: a 5.5 ms median with a 7.5 ms spike every ~112 ms, landing on
  whichever pass is running. The report cannot tell such a frame from one of its own.
