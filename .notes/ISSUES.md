# Open issues
- `SceneBuffers::shade()` rewrites the whole material, layer and mask tables into BAR memory on every frame in which any material animates, because one `setMaterial` moves the shading revision; the masks are megabytes and nothing in them changed.
- `WavePass::record` synthesises the sea every frame in cells with no water (`mWaterLevel` is −∞), 0.2 ms of GPU in every interior.
- The terrain composite bake runs on the main thread inside `SceneUploader::hand` at 16 rows a frame, so a cell load is followed by ~20 s of 1–2 ms frames, and each completion goes through a fenced texture upload that costs a ~15 ms frame.
