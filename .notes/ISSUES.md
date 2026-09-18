# Open issues

- `shot --views=all --against=<its own earlier run>` on one binary is not the same twenty-three
  pictures twice. One pair moved `island-crossing` (worst 14 of 255 on 21% of the pixels), a
  second `island-crossing-end` and `one-cell-walk`, a third `dagon-fel` (13 of 255 on 25%) and
  `dagoth-ur-caldera` (14 of 255 on 46%), and which views move changes from pair to pair. The
  same views drawn as a list of two or three of their own repeat to the byte across runs and
  across binaries. So a view drawn inside the full sequence depends on something the views
  before it left behind, and `repeat` never sees it because `repeat` walks one view in a
  process of its own.

- `check`'s `queue-held` failed once at `balmora-mages-guild` under the gate — "8.452 ms held
  at the median frame of 8.0 asked, 8.452 at the shortest, on 2 of 2 frames", the card at 2325
  MHz with throttle reason 0x400 — and passed at 8.004 on the run after and at every other
  place of the same run. A hold measured over two frames at a place is one reading, and a
  reading a twentieth long fails a check written for a hold that came out at a fifth.
