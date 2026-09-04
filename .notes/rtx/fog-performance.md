# Fog performance and image comparison

2026-09-05. NVIDIA GeForce RTX 4090 Laptop GPU, driver 610.57.04. Release (`-O3 -DNDEBUG`),
1920×1080 internal and output, upscaling off, validation off, SER/reorder off. Builds use ccache.

The retained implementation integrates between the 64 depth texel centres: 63 interior intervals
and two clamped boundary stretches. This preserves the analytic height, water-boundary and directional
transport while reducing the quadrature of sampled coverage and lighting. Filtered directional
visibility uses RG16 instead of RGBA16, saving 7.91 MiB at this resolution.

## Measurement

Each leg covers three moving views, 180 warmup frames and 1200 measured frames per view.
One complete warmup leg is discarded. The measured order is old, accuracy fix, reviewed, reviewed,
accuracy fix, old, with no cooldown between legs. The ship and night cameras move 160 units along X
at 8 units/s; the interior camera moves 40 units along X at 2 units/s. Look targets move with them.
The ship route crosses the exterior cell boundary at X = -8192.

```sh
./openmw-rtxtool bench --views=perf-ship,perf-night,perf-room --size=1920x1080 \
  --upscale=off --window=false --validation=false --seconds=20 --warmup=3 --json=result.json
```

The executables, shaders, exact `resources/rtx/views.cfg` routes and raw captures are under
`/tmp/openmw-fog-perf/{before-fix,before-review,aligned}`; measured JSON files end in `-10.json`
and `-11.json`. Baseline source snapshots were compiled with the same Release flags and current
unaffected objects. FogVolume, Image, VisibilityPass, VulkanRenderer and the harness main were rebuilt
from each baseline, including the types whose layout changed; every baseline shader was rebuilt.

All numbers below are milliseconds. Ranges show the two measured runs, not pooled percentiles.

| Version | View | Frame median | Frame p99 | Worst frame | GPU trace median |
| --- | --- | ---: | ---: | ---: | ---: |
| Old fog (`c84dccffe9`) | Seyda Neen, Foggy | 9.36–10.13 | 14.76–14.84 | 137.22–142.63 | 3.29–3.39 |
| Old fog (`c84dccffe9`) | Balmora, Foggy, 23:00 | 7.94–8.25 | 12.74–12.90 | 20.75–24.04 | 1.96–1.98 |
| Old fog (`c84dccffe9`) | Balmora Mages Guild | 8.77–9.06 | 13.15–13.18 | 15.12–15.22 | 3.69–4.08 |
| Accuracy fix, 128 intervals (`e409be9c95`) | Seyda Neen, Foggy | 10.03–11.07 | 13.93–16.10 | 126.88–128.54 | 4.28–5.05 |
| Accuracy fix, 128 intervals (`e409be9c95`) | Balmora, Foggy, 23:00 | 9.14–9.54 | 13.78–14.72 | 21.77–22.72 | 3.11–3.32 |
| Accuracy fix, 128 intervals (`e409be9c95`) | Balmora Mages Guild | 8.93–9.24 | 13.08–13.61 | 15.25–15.76 | 3.92–3.97 |
| Reviewed fog, 65 intervals | Seyda Neen, Foggy | 10.02–10.32 | 14.70–15.47 | 131.36–131.69 | 4.26–4.38 |
| Reviewed fog, 65 intervals | Balmora, Foggy, 23:00 | 8.66–9.13 | 13.25–13.82 | 22.51–22.69 | 2.64–2.75 |
| Reviewed fog, 65 intervals | Balmora Mages Guild | 8.65–9.19 | 12.52–14.02 | 13.68–15.64 | 3.46–3.95 |

The mean of the two per-run GPU trace medians is 4.66 → 4.32 ms by day and 3.21 → 2.69 ms
at night: savings of 0.34 and 0.52 ms against the original accuracy fix. Relative to the old fog,
the remaining trace cost is about 0.98 ms by day and 0.72 ms at night. These are trace-pass
differences, not whole-frame speedups. Interior results are within the observed run-to-run spread.

Measured GPU clocks span 1770–2055 MHz with 9001 MHz memory and 69–73 °C; the driver reports
software power limiting. Daytime and interior timings show appreciable run-to-run variation.
The ship route has 127–143 ms worst frames across all implementations. That existing cell-crossing
spike remains; this change does not establish smooth streaming or eliminate the accuracy fix’s cost.

## Image comparison

Images use 16 accumulated frames, fixed exposure 1, upscaling and validation off. The reference
uses 512 intervals on the same coverage/lighting grid; it isolates integration error, not the error
in the grid itself. A repeated 128-interval night capture is byte-identical to its control.

| View at 960×540 | Mean absolute RGB difference, 8-bit levels | Maximum channel difference | Pixels over one level |
| --- | ---: | ---: | ---: |
| Seyda Neen, Foggy | 0.00618 | 1 | 0 / 518400 |
| Balmora, Foggy, 23:00 | 0.01594 | 4 | 757 / 518400 |
| Balmora Mages Guild | 0.00077 | 1 | 0 / 518400 |
| Dagoth Ur, Blight | 0.00435 | 1 | 0 / 518400 |

At 1920×1080, the night comparison has a mean absolute difference of 0.01619 display levels,
a 99th-percentile channel difference of 1, and a maximum of 4.
3569 of 2073600 pixels differ by more than one level.

A simple reduction to 64 intervals split at slice edges missed narrow lamp peaks: the night
comparison reached 25 display levels of error. Aligning intervals to interpolation slope changes
reduces that to four. The 128-interval version stays within one level of the dense night reference.
The retained compromise therefore has a small, localized lamp-halo difference; it is not pixel-identical.

Combining the whole-ray and particle-prefix integrals into one traversal was measured slower
and was discarded. Retaining the two calls avoids that regression. The CPU frame path adds no
allocation; the scoped warm-frame allocation tests pass.

## Verification

Debug targets: `components-tests`, `openmw-rtxtool`, `openmw`. Release target: `openmw-rtxtool`.
Shaders pass `spirv-val`; changed C++ and shared headers pass clang-format 14. The 52 scoped tests
cover analytic transport, fog, sprites, sky, water, GPU timings and frame allocations, with no skips
or validation errors (7.66 seconds in the final run). Sunlight and both moons share the analytic
and lid-shadow sweeps. The fog composition test includes a half-opaque black sprite before, at and after integration boundaries.
The GPU-timer test took 1.745 seconds including renderer/pipeline startup; that existing fixture issue
is recorded in `../ISSUES.md`.
