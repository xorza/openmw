# Open issues

- `AGENTS.md` states the picture residual as "about three pairs in ten differ, always on 26 or 27
  frames of 45". `repeatable.sh` now walks 360 frames, and six pairs at `ce607d584c` differed on 204
  to 343 of them. Every pair differed.

- `SlotTable::resize` shrinks the row count and returns without touching the debts. A copy that
  already owed a row past the new end still owes it, and `sync` then reads `mRows[at]` past the end.
