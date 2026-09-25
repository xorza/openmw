# Open issues

- `rtx release gate` runs `components-tests`, `rtx-gpu-tests` and `openmw-tests` whenever the binaries exist, but the release flavour's `targets` does not name them, so the gate builds none of them. A gate after a test or shader-side change reports the results of stale test binaries: a new test in `light.cpp` was absent from a "gate: clean" run.
