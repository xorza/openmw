# Open issues
`build-release` does not compile: `components/rtx/cellring.cpp:216` fails
`-Werror=null-dereference` under GCC 16 at `-O3`, and the same diagnostic fires inside
`<bits/stl_iterator.h>` for `CellRing::adopt`. `build-debug` at `-O2` is clean.
