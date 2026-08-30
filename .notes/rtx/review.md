# Review of the RTX fork against `upstream/master`

Merge base `d720b4c82ccfa1f4d89353699929545945d79a2c`. The diff is 615 files, +92145/−4906.
The RTX-owned places hold about 385 files and 79,900 lines. The upstream-owned places hold 216
files, +10257/−4844.

Delete an item when it is addressed. Delete a heading when it is empty. This file lists open items
only.

Each item describes what is there and what it costs. No item says how to fix it.

## Work is done per frame that the frame did not change

- [ ] `components/rtxvulkan/guitextures.*` is synchronous. `getView`, `read` and `writeWith` all
      call `flush()`, which is a submit and a wait. `drawGui` calls `getView` per batch. A texture
      written in a frame costs one submit-and-wait inside that frame. `videowidget.cpp` writes one
      texture per frame.

## Harness command plumbing is copied per command

- [ ] `apps/rtxtool/bench.cpp` `runBench` mixes timing, hashing, JSON, crossing accounting and
      reporting in one function.

