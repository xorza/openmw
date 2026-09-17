# A picture without Ray Reconstruction

What `upscale = off` shows today, why, what shipped path tracers do without Ray Reconstruction,
and what it would take here. Research, not a plan: nothing below has been decided.

## What is noisy today

`Rtx::Reconstruction::resolve` hands an unupscaled frame to the wavelet, and the wavelet
(`AccumulatePass` + `AtrousPass`) runs over `CHANNEL_INDIRECT` only. Every other random term of
the trace reaches the screen at one sample per pixel, all of them inside `CHANNEL_DIRECT`:

- **Which lamp lights a pixel.** `Reservoir` (`lights.glsl`) keeps one lamp per pixel out of every
  lamp that reaches it, by one-deep reservoir sampling. A room with six lanterns gives each pixel
  one of them.
- **One shadow ray per source, across its disc.** `lampVisible` aims one ray across the lamp's
  `mSourceRadius`; `skyVisible` aims one across the sun's penumbra (`SUN_SHADOW_RADIUS`, 2°). A
  penumbra is one sample per pixel.
- **The sun-or-moon draw** in `gather`, and **one lamp per pane layer** (`SEED_LAMPS_PANE`,
  `SEED_LAMPS_PANE_DEEPER`): a paper screen draws a lamp for each layer.
- **No anti-aliasing.** `mJitter` is off by default and no pass resolves a jittered history.

The bounce is filtered and the fog reprojects its froxels; those two are fine. The budgets
`VisibilityConstants::mBounceRate` and `INDIRECT_LIGHT_RATE = 0.5` add variance to the bounce that
the wavelet was tuned to carry under Ray Reconstruction.

Seen with:

```
openmw-rtxtool shot --views=balmora-mages-guild,balmora --upscale=off --filter=true --out=<a>
openmw-rtxtool shot --views=balmora-mages-guild,balmora --upscale=dlaa --out=<b>
```

The guild interior is the case: every wall near a lantern and both paper screens are speckled
under `off`, and clean under `dlaa`. The noon exterior shows it only at penumbra edges and near
lamps.

## What games do

The denoiser is never a menu item. "DLSS off" in a game means no upscaler; the denoiser and TAA
stay on. Ray Reconstruction replaces the game's denoiser when it is on, and the game's own stack
runs when it is off. That stack is NVIDIA NRD in more than fifteen AAA titles — Cyberpunk 2077
runs ReLAX, Portal RTX and Alan Wake 2 run NRD too.

The NRD stack, and NVIDIA's stated best practice (NRD README and NRD-Sample):

- **ReBLUR** (recurrent blur) or **ReLAX** (à-trous) over demodulated diffuse and specular
  radiance. ReLAX was designed for RTXDI output, ReBLUR for a plain path tracer.
- **SIGMA** over the sun's shadow: per light, penumbra-aware, from a visibility ray's distance.
- Inputs: demodulated radiance, hit distance, normal + roughness, motion vectors, linear view
  depth, optional history confidence. The RR path here writes all of these already except the hit
  distance.
- Order: denoise → SG/SH resolve → upscale → TAA or DLSS. One ray per pixel with a probabilistic
  diffuse/specular split, clamped to `[1/4, 3/4]` with Bayer dithering; blue noise with temporal
  rotation.
- NRD's README: its SH mode "achieves quality comparable with DLSS-RR".
- Variance reduction before the denoiser: **ReSTIR / RTXDI** reuses the lamp reservoir across
  frames and neighbours. `Reservoir` here is already the record for it ("what would get carried,
  if carrying it were worth anything"). The "not worth building" measurement was made under RR,
  which hides the selection noise.

Quake II RTX (GPL, open) is the in-house version of the same stack (`asvgf.glsl`): A-SVGF over
direct diffuse (HF) with gradient-based history rejection; a cheap 1/3-resolution spherical-
harmonic filter for indirect diffuse (LF); temporal-only filtering for specular, because spatial
filters failed on normal-mapped surfaces; then TAA + upscale (`asvgf_taau`).

A hand-rolled SVGF is "roughly 700 lines of shader to work, 2,000 to be good".

## What it takes here

Two routes. Both need the last two items.

**A. Extend the tree — GPL-clean, the Q2RTX shape.**

1. Split `CHANNEL_DIRECT`: direct diffuse irradiance (lamps + sun, demodulated) into a channel of
   its own. Sky, emission, water and fog stay resolved and pass the filter by.
2. Run `AccumulatePass` + `AtrousPass` over the direct channel too. Cost at 1080p: about the
   current `filter 1.67 ms` + `accumulate 0.30 ms` again. Direct is higher-frequency than the
   bounce — a penumbra is an edge — so it wants fewer levels and a tighter variance guide.
3. In this mode, trace every bounce and every indirect light (`mBounceRate = 1`, the
   `INDIRECT_LIGHT_RATE` draw off): a wavelet carries less variance than RR.

**B. NRD.** ReBLUR/ReLAX + SIGMA in one library, with a hit-distance channel added to the
G-buffer. Under the NVIDIA RTX SDKs licence — not MIT since v4 — the same standing as the NGX SDK
the build already names rather than vendors. A new dependency, and needs a go-ahead.

**Both routes need:**

4. **ReSTIR temporal reuse.** Reproject last frame's reservoir by `CHANNEL_MOTION`, combine it with
   this frame's by the rule that built it (`considerLamp`), trace one shadow ray. This turns the
   interior speckle into shading before any filter sees it. Spatial reuse is the second step and
   optional.
5. **A TAA resolve pass** over the jittered frame, using the motion channel: about 0.2 ms.
   Without it, jitter on is shimmer and jitter off is aliasing.

Recommendation: NRD, if the dependency is accepted. It is what every shipped path tracer runs
without RR, and the in-tree route would be a second hand-made denoiser next to a library that
already exists. Either way the shipping path stays RR; this is the path for a machine or a
reference that will not run it.

## Sources

- NVIDIA-RTX/NRD — https://github.com/NVIDIA-RTX/NRD
- NRD licence — https://raw.githubusercontent.com/NVIDIA-RTX/NRD/master/LICENSE.txt
- NRD-Sample README — https://github.com/NVIDIA-RTX/NRD-Sample/blob/main/README.md
- NVIDIA, DLSS 3.5 Ray Reconstruction in Cyberpunk 2077 —
  https://www.nvidia.com/en-gb/geforce/news/dlss-3-5-available-september-21
- NVIDIA/Q2RTX `asvgf.glsl` —
  https://github.com/NVIDIA/Q2RTX/blob/master/src/refresh/vkpt/shader/asvgf.glsl
- Q2VKPT — https://brechpunkt.de/q2vkpt/
