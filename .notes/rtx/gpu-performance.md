# The device side of the RTX frame

What the card spends a frame on, and what that says about the budget. Taken at `4f2b88db9d` with a
clean working tree, on an RTX 4090 Laptop and an i9-13980HX, NVIDIA 610.57.04. The build is
`apps/rtxtool/release.sh`.

Every reading is `bench --validation=false --window=false --seconds=20`, which is 1200 measured
frames after 180 warm-up frames at each place. The card was warm before the first suite, and no run
waited for it to cool. Unless a row says otherwise, the output is 1920×1080 and the upscaler is at
quality, which traces 1280×720.

**The numbers are the device's own.** `bench` closes a timestamp query around each pass and prints
what the card reported, so a zone is what the card took and not what the host waited. Every place
that stands still waits on the device, so these zones are the frame's budget.

## What each zone is

| zone | what it is |
|---|---|
| `upscale` | DLSS Ray Reconstruction, between the trace and the picture |
| `trace` | the ray-tracing pipeline: the picture |
| `air` | the fog volume's depth ray a column, and the scatter through its froxels |
| `column` | the same volume integrated along each column |
| `waves` | the sea's spectrum, synthesised once a frame for every ray |
| `bloom` | what the lens spreads |
| `composite` | where the bounce ended up, gathered for the upscaler |
| `tone`, `exposure` | the curve, and the measurement it is driven by |
| `shade`, `sprites` | the emitters' lighting and their screen-space bin |
| `tlas` | the top level, rebuilt every frame |
| `refit`, `skin` | the bottom levels a pose moved, and the pose itself |
| `micromap`, `blas`, `compact` | what a cell arriving builds |

## Where it stands

Milliseconds, medians, at 1920×1080 out and 1280×720 traced. `gpu` is the sum of the zones.

| view | frame | wait | gpu | upscale | trace | air | column | waves | tlas | refit | bloom | rest |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| vivec | 6.84 | 3.53 | 5.78 | 2.27 | 1.97 | 0.16 | 0.08 | 0.21 | 0.23 | 0.33 | 0.12 | 0.41 |
| seyda-neen-ship-overcast | 5.87 | 3.83 | 5.16 | 2.35 | 1.56 | 0.23 | 0.10 | 0.22 | 0.24 | 0.13 | 0.13 | 0.20 |
| seyda-neen-ship | 5.81 | 3.79 | 5.07 | 2.31 | 1.52 | 0.23 | 0.10 | 0.22 | 0.24 | 0.13 | 0.13 | 0.19 |
| balmora-storm-night | 5.63 | 3.35 | 4.91 | 2.42 | 1.24 | 0.16 | 0.10 | 0.22 | 0.26 | 0.16 | 0.13 | 0.22 |
| seyda-neen-shore | 5.58 | 4.04 | 5.13 | 2.45 | 1.50 | 0.25 | 0.10 | 0.22 | 0.26 | 0.06 | 0.13 | 0.16 |
| balmora | 5.42 | 3.37 | 4.76 | 2.37 | 1.20 | 0.17 | 0.09 | 0.22 | 0.26 | 0.15 | 0.13 | 0.17 |
| ald-ruhn | 5.41 | 3.16 | 4.69 | 2.36 | 1.01 | 0.20 | 0.11 | 0.22 | 0.24 | 0.18 | 0.13 | 0.24 |
| balmora-fog-night | 5.40 | 3.15 | 4.61 | 2.39 | 1.04 | 0.16 | 0.09 | 0.22 | 0.25 | 0.16 | 0.12 | 0.18 |
| dagon-fel | 5.27 | 4.06 | 4.83 | 2.48 | 1.20 | 0.17 | 0.10 | 0.23 | 0.24 | 0.08 | 0.13 | 0.20 |
| sadrith-mora | 5.25 | 3.19 | 4.54 | 2.40 | 0.86 | 0.19 | 0.11 | 0.22 | 0.26 | 0.18 | 0.13 | 0.19 |
| addamasartus | 5.15 | 4.06 | 4.71 | 2.52 | 1.22 | 0.10 | 0.07 | 0.23 | 0.18 | 0.09 | 0.13 | 0.17 |
| balmora-mages-guild | 5.01 | 3.92 | 4.54 | 2.55 | 1.25 | 0.10 | 0.06 | 0.00 | 0.20 | 0.10 | 0.13 | 0.15 |
| seyda-neen-customs | 4.62 | 3.64 | 4.22 | 2.56 | 0.95 | 0.09 | 0.06 | 0.00 | 0.18 | 0.06 | 0.13 | 0.19 |
| andrano-tomb | 4.55 | 3.33 | 4.10 | 2.57 | 0.82 | 0.10 | 0.06 | 0.00 | 0.17 | 0.08 | 0.13 | 0.17 |
| vivec-canalworks | 4.53 | 3.63 | 4.10 | 2.45 | 0.90 | 0.11 | 0.09 | 0.00 | 0.15 | 0.05 | 0.11 | 0.24 |
| arkngthand | 4.13 | 3.29 | 3.74 | 2.57 | 0.41 | 0.13 | 0.10 | 0.00 | 0.18 | 0.05 | 0.13 | 0.17 |
| island-crossing | 5.37 | 3.48 | 5.74 | 2.34 | 0.83 | 0.20 | 0.09 | 0.21 | 0.21 | 0.08 | 0.12 | 1.66 |

The crossing's `rest` is its arrivals: a `micromap` bake of 0.81 ms a frame and a `blas` build of
0.69, spread over the frames a ring lands on.

**The upscaler is the largest zone at every one of the seventeen.** It is 39 per cent of the device
frame at Vivec, 46 at Seyda Neen and 69 in the Dwemer ruin. It is also the flattest: 2.27 to 2.57 ms
whatever is in front of the camera, because it is a function of the extent and not of the scene.

## The one law: both costs are linear in the pixels traced

The upscaler's ladder at `seyda-neen-ship`, output 1920×1080 throughout. `off` is the fork's own
wavelet instead of Ray Reconstruction, which traces the whole output.

| mode | traced | Mpx | trace | upscale | frame | trace ns/px | upscale ns/px |
|---|---|---:|---:|---:|---:|---:|---:|
| ultraperformance | 640×360 | 0.230 | 0.42 | 0.80 | 2.90 | 1.82 | 3.47 |
| performance | 960×540 | 0.518 | 0.84 | 1.29 | 3.96 | 1.62 | 2.49 |
| balanced | 1114×626 | 0.697 | 1.14 | 1.77 | 4.80 | 1.64 | 2.54 |
| quality | 1280×720 | 0.922 | 1.54 | 2.33 | 5.82 | 1.67 | 2.53 |
| dlaa | 1920×1080 | 2.074 | 3.44 | 5.38 | 11.22 | 1.66 | 2.59 |
| off, wavelet | 1920×1080 | 2.074 | 3.04 | 1.99 | 7.32 | 1.47 | 0.96 |

**The trace costs 1.7 nanoseconds a pixel and Ray Reconstruction costs 2.5, and both hold across a
nine-fold range of extents.** So the budget is arithmetic: pick an extent and the device frame
follows. The two 4K rows below were taken separately and land on the same two constants, which is
what says the law is the hardware's rather than one scene's.

**And the denoiser costs half again what the ray tracing costs.** That ratio is 1.5 at every mode
but the smallest, where a fixed cost inside the upscaler still shows.

**The fork's own wavelet is a third of the price per pixel** — 0.96 against 2.53 — and it is not
what the fork uses, because Ray Reconstruction is the picture the fork is for. What the table says
is the size of that decision, not that it is wrong.

## Finding 1 — the stated target is met, and the denoiser is half of it

`AGENTS.md` states the target as 1920×1080 internal to 3840×2160 at 60 fps. That is
`--size=3840x2160 --upscale=performance`, which traces exactly 1920×1080.

| view | frame | gpu | upscale | trace | fps | 1% low |
|---|---:|---:|---:|---:|---:|---:|
| balmora-mages-guild | 11.04 | 11.15 | 5.98 | 3.13 | 90.6 | 56.7 |
| seyda-neen-ship | 11.96 | 11.39 | 5.31 | 3.47 | 83.6 | 65.4 |
| vivec | 13.38 | 12.52 | 5.26 | 4.53 | 74.8 | 58.9 |

**The target is met at the median with three to six milliseconds to spare, and the one per cent low
sits on the line** — 56.7 to 65.4 fps against the 60 the target names.

Ray Reconstruction is 5.26 to 5.98 ms of that, which is 45 to 54 per cent of the device frame and
between 1.2 and 1.9 times the trace.

**Quality upscaling to 4K does not reach the target.** It traces 2560×1440 and reads 45.7 to 55.3
fps, with the upscaler alone at 9.48 to 10.29 ms. The mode is the budget.

## Finding 2 — the flat costs are a fifth of an interior's frame

Six zones barely move with the scene: `waves` 0.21–0.23 wherever the cell has water, `tlas`
0.15–0.26, `bloom` 0.11–0.13, `composite` 0.03–0.05, `tone` 0.04–0.06 and `exposure` 0.02–0.04.

Together they are 0.70 ms of an exterior frame and 0.42 of the Dwemer ruin's — 14 per cent at Seyda
Neen and 11 in the ruin, where the trace itself is 0.41 ms.

**`waves` is the one to look at first.** It is a spectrum synthesised once a frame for a sea that a
cell may show a sliver of or none at all, and it costs the same 0.22 ms either way. It is skipped
where a cell holds no water, which is what makes the interiors read nought — so the question is not
whether it can be skipped but whether it has to run on a frame where the sea moved by nothing a
pixel could show.

**And `tlas` is rebuilt every frame.** 0.24 ms is five per cent of an exterior's device frame. The
code says a rebuild costs an arrival nothing because it happens regardless; what it does not say is
what a refit would cost on the frames where only transforms moved.

## Finding 3 — Shader Execution Reordering is still slower

Measured again at Vivec, which has the heaviest trace in the corpus, because the shaders have been
rewritten since the reading `AGENTS.md` records.

| mode | trace | frame |
|---|---:|---:|
| off | 1.85 | 6.59 |
| hit | 2.20 | 6.94 |
| hint | 2.22 | 6.93 |
| both | 2.21 | 6.93 |

**Nineteen per cent slower, and the three modes are indistinguishable from one another.** The
default stays off, and the decision is now measured against the current shaders rather than inherited.

## Finding 4 — opacity micromaps buy three per cent of the trace

At `seyda-neen-ship`, which carries 1369 cutout instances and micromaps every one of them:

| leg | trace | frame |
|---|---:|---:|
| micromaps on | 1.54 | 5.85 |
| micromaps off | 1.59 | 5.90 |
| micromaps on | 1.56 | 5.86 |
| micromaps off | 1.86 | 5.99 |

The first pair is the clean one: 0.05 ms of a 1.54 ms trace. The second `off` leg carried a 320 ms
worst frame and is the leg to repeat rather than to read.

**They cost 23.4 MiB of micromap memory and a bake on the frames a cell arrives on** — 0.81 ms a
frame over the island route. On a standing camera they are three per cent of the trace. That is a
trade worth re-reading when the corpus has a place with far more foliage than the ship deck.

## Finding 5 — the Ray Reconstruction preset does not move the cost

`preset d` against `preset e` at `seyda-neen-ship`, one leg each: `upscale` 2.37 against 2.30 and the
frame 5.87 against 5.80. That is inside the run-to-run spread, so the choice is a picture question
rather than a cost one.

## What the tools can and cannot say

**Nsight Systems needs `--trace=vulkan-annotations`, and it needs a build that labels.** Plain
`--trace=vulkan` records no debug-utils label at all, so `vulkan_gpu_marker_sum` comes back empty and
`vulkan_marker_sum` says the capture holds no such data. That is the switch and not the fork.

The labels are `GpuTimer::open`'s own — every zone this document names is already one — and
`components/rtxvulkan/CMakeLists.txt` compiles them out of `Release`. So a timeline is available on
`build-debug` and not on the build the numbers come from:

```
nsys profile --trace=vulkan-annotations --sample=none \
    build-debug/openmw-rtxtool bench --validation=false --window=false --views=seyda-neen-ship --seconds=4
nsys stats --report vulkan_gpu_marker_sum <capture>.nsys-rep
```

**What it adds over the fork's own timer is one row.** Inside the `upscale` zone sits
`nv.ngx.dlssd.Evaluate`, and it is 1.772 ms of the zone's 1.775 — so what the upscaler costs is the
network's and not this fork's binding of it.

**And what it cannot be trusted for is the ranges themselves.** `waves` reads 0.223 ms against the
timer's 0.22, and `trace` reads 8.6 microseconds against the timer's 1.5 ms. A label bounds a
recording rather than the work the card then does asynchronously inside it, so the timestamps stay
the authority and nsys is for the shape.

What the host trace added: 47 pipeline barriers a frame at 205 ns of host time each, and a
`vkQueuePresentKHR` on every frame even under `--window=false`.

**What a pass costs inside the trace kernel needs the pass removed.** `ncu` is not installed, and the
trace is one dispatch — so the bounce, the caustics, the shadow rays and the sea are one number here.
Pricing them apart is a build per pass and a `shot` each.

## What to do next, in order

1. **Ask what the sea's spectrum costs on a frame that does not need it.** 0.22 ms every frame with
   water in the cell, whatever the camera can see of it.
2. **Price a top-level refit against the rebuild.** 0.24 ms a frame, on frames where only transforms
   moved.
3. **Price the trace's own passes by removing them.** The trace is 0.41 to 4.53 ms depending on the
   place and the extent, and it is one number today.
4. **Leave the upscaler alone unless the picture changes.** It is the largest cost in every frame and
   it is a fixed function of the extent — the only knob on it is which extent to trace, and the
   target already names one that fits.

## How to repeat these measurements

```
cd build-release
for suite in default exteriors skies interiors; do
    ./openmw-rtxtool bench --validation=false --window=false --suite=$suite --seconds=20
done
./openmw-rtxtool bench --validation=false --window=false --suite=streaming --seconds=20 --settled=false

for mode in off ultraperformance performance balanced quality dlaa; do
    ./openmw-rtxtool bench --validation=false --window=false --views=seyda-neen-ship --seconds=20 --upscale=$mode
done

./openmw-rtxtool bench --validation=false --window=false --views=seyda-neen-ship --seconds=20 \
    --size=3840x2160 --upscale=performance
```

Warm the card with one thrown-away `bench` first. The clock and the temperature are printed beside
every result, and a leg whose clock differs from its neighbour's is the leg to repeat.
