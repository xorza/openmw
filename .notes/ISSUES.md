# Open issues

- `BenchRecord`'s scene JSON (`asJson(const SceneStats&)` in `components/rtxbench/benchrecord.cpp`) claims every field of `SceneStats` and leaves out `mCompactableBytes` and `mCompactableNowBytes`.
