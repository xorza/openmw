# G-buffer and history precision: investigation and plan

`.notes/rtx/shader-review.md` §4 asks for half floats for the radiance and the history, an
octahedral normal beside a roughness, and full floats only where a second moment is accumulated.
This is that finding measured against the tree, corrected in three places, and staged.

The short of it: **the accumulator runs at this part's memory limit and the cascade at half of it,
while the trace runs at a tenth.** So the finding is right about where the bandwidth is and wrong
about which channels can move — the two radiance channels are already closed by a measurement in
`gbuffer.cpp`, and a world distance does not fit in a half at all. What is left is still worth
about three fifths of the cascade and a quarter of the accumulator.

## 1. What the frame costs today

Release build at `ee105f35ba`, this box — RTX 4090 Laptop, 256-bit GDDR6 at 18 Gbps, **576 GB/s
peak** — the default `bench` suite, 1200 frames a view, median GPU zone.

**The shipping frame** (`--upscale=quality`): 1920×1080 shown, 1280×720 traced.

| view | trace | upscale |
|---|---|---|
| seyda-neen-ship | 1.63 | 2.05 |
| seyda-neen-ship-dawn | 2.03 | 2.04 |
| balmora-mages-guild | 1.55 | 2.13 |

**The wavelet frame** (`--upscale=off`): 1920×1080 traced and shown.

| view | trace | accumulate | filter |
|---|---|---|---|
| seyda-neen-ship | 3.43 | 0.57 | 2.31 |
| seyda-neen-ship-dawn | 4.47 | 0.57 | 2.33 |
| balmora-mages-guild | 3.65 | 0.68 | 2.81 |

### 1.1 What the trace writes

| channel | format | bytes |
|---|---|---|
| `CHANNEL_DIRECT` | `rgba32f` | 16 |
| `CHANNEL_INDIRECT` | `rgba32f` | 16 |
| `CHANNEL_ALBEDO` | `rgba16f` | 8 |
| `CHANNEL_SPECULAR` | `rgba16f` | 8 |
| `CHANNEL_GUIDE` | `rgba32f` | 16 |
| `CHANNEL_MOTION` | `rg32f` | 8 |
| `CHANNEL_DEPTH` | `rg32f` | 8 |
| `CHANNEL_REFLECTION_MOTION` | `rg32f` | 8 |
| `CHANNEL_PARTICLE_MASK` | `r8` | 1 |
| `CHANNEL_BIAS_MASK` | `r8` | 1 |
| `CHANNEL_STARS_SHOWN` | `rgba8` | 4 |
| | | **94** |

### 1.2 What each pass moves, and against what

Compulsory traffic — every image each pass touches, counted once a pixel, which is what reaches
DRAM once the taps have been served by the caches.

**The trace**, at 1280×720: 94 bytes written, 86.6 MB a frame, **53 GB/s over 1.63 ms — 9% of
peak.** The review's "the trace itself is not bandwidth-bound" is right, and by an order of
magnitude.

**The upscaler**, at 1280×720 in and 1920×1080 out: 74 bytes read a traced pixel (colour 16, guide
16, albedo 8, specular 8, depth 8, motion 8, reflection motion 8, two masks 2) and 16 bytes written
an output pixel. 101 MB a frame, **49 GB/s over 2.05 ms — 9% of peak.** NGX is compute-bound here,
so nothing in this plan will move the `upscale` zone.

**The accumulator**, at 1920×1080: 89 bytes read a pixel (indirect 16, guide 16, depth 8, bias mask
1, and the three histories at 16 apiece) and 64 written. Sky pixels take the early return, so at
Seyda Neen's 85% hit rate that is about 143 bytes, 297 MB a frame, **521 GB/s over 0.57 ms — 90% of
peak.** This pass is at the memory limit and nowhere near anything else.

**The cascade**, at 1920×1080, per level: guide 16, depth 8, source 16 and moments 16 read, 16
written — 72 bytes, 360 over the five levels, 747 MB a frame, **323 GB/s over 2.31 ms — 56% of
peak.** Its *tap* traffic is a different and larger number: 25 taps of guide, depth and source is
1000 bytes a level and 5160 over the cascade, which at 2.074 M pixels is 10.7 GB a frame and 4.6
TB/s — served by L1 and L2, and the reason a narrower tap is worth more here than the compulsory
figure suggests.

## 2. Three corrections to §4

### 2.1 The two radiance channels are closed, and the reason is sound

`gbuffer.cpp` already records the experiment §4 proposes: the converged mean of a flat surface came
back 0.096% low against a tolerance of 0.067%. The test is
`RtxVisibilityTest.aBounceDrawsItsDirectionByTheCosineAndNotUniformly` — a floor of albedo 0.5 under
a sky running horizon to zenith, 64 accumulated frames, `EXPECT_NEAR(mean, 0.3, 0.0002f)`.

The mechanism is worth stating exactly, because it is not the one a reader assumes. Round-to-nearest
into a half is unbiased over a spread of values, so quantising 64 independent samples would average
out. These are not independent: the bounce draws `d.z = sqrt(1 - u)` from a low-discrepancy sweep of
`u`, so a pixel's 64 values are evenly spaced across the interval — and evenly spaced samples of the
sawtooth that a rounding error is do not cancel, they alias. That is what a quarter of a per cent of
bias looks like.

**So `CHANNEL_DIRECT` and `CHANNEL_INDIRECT` stay `rgba32f`, and nothing below asks otherwise.**
What §4 wanted from them is reached instead by decoupling the denoiser's working images from them,
which §6 does.

### 2.2 A world distance does not fit in a half

`Rtx::sFarPlane` is 200000 and the fixtures use 100000. A half's largest finite value is 65504, so
the distance in `historySurface.w` — and any distance a packed guide would carry — becomes infinity
past that, `abs(inf - inf)` is a NaN, and every comparison against it is false. The pixel silently
loses its history and stays noisy, in exactly the distance where the cascade has least to work with.

A half's *relative* step is fixed at 2⁻¹¹, so the fix is a scale and not more bits: store
`distance / mFar`, which is in [0, 1] by construction and keeps the same 0.049% relative precision
everywhere. `Camera` deliberately carries no `mFar` — `camera.h` says why — so the scale belongs in
`AccumulateConstants` and `AtrousConstants`, which is where a pass's own storage decision belongs
anyway.

The cascade's own reconstruction is scale-free under a pinhole and is not under a parallel
projection: `positionAt` is `mOffset + mDirection * away` with `mOffset` zero for a pinhole and a
world position for a map tile, and `footprint` is `mSpread * away` for one and a constant `mWidth`
for the other. So a normalised distance cannot be left un-scaled on the assumption that the ratio
survives. It does for the game's frames and not for `map` or `doll`.

### 2.3 The accumulator and the cascade do not run in the shipping frame

`Reconstruction::resolve` answers `RayReconstruction` whenever anything upscales, and
`vulkanrenderer.cpp` runs `recordDenoise` only for `Denoiser::Wavelet`. `upscale = quality` is the
default in `settings-default.cfg`. **So every millisecond §4 aims at is a millisecond of the path
with no DLSS on it** — the A/B against the upscaler, a Metal backend, and any machine without Ray
Reconstruction.

That is not a reason to skip the work, and it is a reason to rank it below anything that touches
`trace` or `upscale`. It also means the `verify` view set and the frame budget in
`.notes/rtx/plan.md` will not move at all: the gate for this work is the `--upscale=off` bench.

## 3. What the field does, and what NGX will take

- **DLSS-RR Integration Guide §3.4.3, Normals**: "RGB16_FLOAT or RGB32_FLOAT provided at input
  resolution", with roughness packed into alpha under
  `NVSDK_NGX_DLSS_Roughness_Mode_Packed`, which `DlssPass` already sets. So a half-float guide is
  named by the guide as supported, not merely tolerated.
- **§3.4.6, Motion Vectors**: "RG16_FLOAT or RG32_FLOAT". Permitted — and `GBuffer::getMotion`'s own
  reason overrides it, because a half lands only on whole pixels above 1024 and a camera turn spans
  more than that. Motion stays `rg32f`.
- **§3.4, Supported Resource Formats**: "DLSS-RR uses formatted reads, therefore it should handle
  most input buffer formats." There is no fixed-format requirement to design around.
- SVGF, A-SVGF, ReLAX and NRD all keep the filtered radiance and the history in half floats and the
  moments in full ones, for the cancellation reason §4 gives. This plan agrees with them on both.
- `VK_FORMAT_R16G16B16A16_SFLOAT` is required to support `STORAGE_IMAGE` on every Vulkan device, so
  §5 and §6 need no probe. `VK_FORMAT_R16G16_SNORM` is not, so §8 does — and fails naming the
  format, the way `gbuffer.h` argues for `R8_UNORM`.

## 4. Stage 0: the passes own their own formats

`AccumulatePass` builds its three history images from `GBUFFER_RADIANCE` and `GBUFFER_GUIDE`, and
`AtrousPass` builds its scratch from `GBUFFER_RADIANCE`. `accumulate.comp`'s own header says "The
history is this pass's own and not the G-buffer's" — and then borrows the G-buffer's formats, which
is how a change to a channel silently changes a history that shares nothing with it but a number of
bits.

Add to `accumulate.h`, beside `ACCUMULATE_FRAMES`:

- `ACCUMULATE_COLOUR` — the running mean and the frame count.
- `ACCUMULATE_SURFACE` — the normal and the distance the mean belongs to.
- `ACCUMULATE_MOMENTS` — the two moments and the variance.

and to `atrous.h`:

- `ATROUS_CHANNEL` — what a level reads and writes.

Each a `RTX_HOST` `VkFormat` and a GLSL layout token, exactly as `gbuffer.h` does it and for the same
reason. All four start at today's values, so this stage is a pure refactor: `verify` is
byte-identical and every test passes unchanged.

**Effort**: an hour. **Risk**: none. It is what makes each stage below one edit in one place.

## 5. Stage 1: the guide to half floats

`GBUFFER_GUIDE` becomes `VK_FORMAT_R16G16B16A16_SFLOAT` / `rgba16f`. One macro pair; the trace, the
accumulator, the cascade and `GBuffer` all follow it. `ACCUMULATE_SURFACE` stays at `rgba32f` and
does not follow, for §2.2's reason — which is what Stage 0 is for.

**What it buys.** The trace writes 86 bytes rather than 94, on a pass at 9% of peak, so expect
nothing there. The accumulator's compulsory read drops 8 of 143 bytes. The cascade's tap traffic
drops 200 bytes a level of 1032 — **19%** — and its compulsory traffic 8 of 72, so expect the
`filter` zone to move by something between the two.

**What it costs.** A normal in halves has 11 bits a component, which is an angular error under
0.03°; `atrous.comp` cuts a tap at about 6° through `pow(dot, 128)` and `accumulate.comp` at 26°
through `SURFACE_FACING`. Three orders of margin. The roughness is a fraction in [0, 1] and NGX is
told it is packed.

**Verification.** `RtxVisibility*` in full — the guide is what every filter test compares surfaces
by. `openmw-rtxtool verify` will move on the wavelet views and must not move on the upscaled ones.
Bench both profiles.

**Effort**: half a day, most of it the measurement.

## 6. Stage 2: the history is half floats, and its distance is normalised

`ACCUMULATE_COLOUR` and `ACCUMULATE_SURFACE` become `rgba16f`. `ACCUMULATE_MOMENTS` stays
`rgba32f` — `E[l²] - E[l]²` is a cancellation and half floats have nothing to spare for one.
`AccumulateConstants` gains the far distance, and `surfaceOut.w` becomes `away / mFar` with
`sameSurface`'s tolerance applied in the same units.

**What it buys.** The accumulator's compulsory traffic falls from 143 bytes a pixel to about 111 —
**22%** — on a pass measured at 90% of this part's peak, so the time should follow almost linearly:
0.57 ms toward 0.45.

**What it costs, and it needs stating.** The running mean is an exponential average with
`alpha = 1/16`, so a frame moves the stored value by 6.25% of the difference. A half's relative step
is at most 2⁻¹¹ and at least 2⁻¹², so an update whose difference is under **0.4 to 0.8% of the
value** rounds back to where it was and the average stops moving. That is a convergence floor, not a bias, and it sits well
inside the 2% the filter tests allow — but it is the number to check first if
`theFilterAndItsHistoryConvergeOnAGrazingSurface` moves, and it is the reason the moments do not
follow.

**Verification.** The six tests in `filter.cpp`, and two of them carry figures rather than
tolerances: `theFilterAndItsHistoryConvergeOnAGrazingSurface` asserts `alone > 0.003` and
`settled < alone * 0.60` against measured values of 0.00380 and 0.00214. **Re-derive those, do not
widen them.** If the settled error rises past 0.60 of the raw one, the floor above is the first
suspect and full-float colour with half-float surface is the fallback.

**Effort**: a day.

## 7. Stage 3: the cascade works in half floats

`ATROUS_CHANNEL` becomes `rgba16f`, and with it the cascade stops writing through
`CHANNEL_INDIRECT`.

**Why the second scratch is the point rather than the cost.** Today `AtrousPass` ping-pongs between
its scratch and `CHANNEL_INDIRECT`, and `accumulate.comp` writes its blend back over the channel it
read — an aliasing the file has to argue for. Both exist only because the passes borrowed the
G-buffer's image. Give `AccumulatePass` a `mBlended` of its own, let the cascade ping-pong `mBlended`
against `mScratch`, and `CHANNEL_INDIRECT` becomes what it says it is: written once by the trace,
read once by whatever consumes it. The reference sum in `composite.comp` then reads a full-float
channel on the path that builds a reference — which is the whole of §2.1's constraint, met by
construction rather than by a format.

**The one obstacle.** `composite.comp` declares its indirect binding with a format qualifier, and it
is now handed `rgba32f` with no denoiser and `rgba16f` with one. Two ways out:

1. **`shaderStorageImageReadWithoutFormat`.** A `readonly image2D` with no format qualifier, which
   `imageLoad` converts from whatever the view holds. A Vulkan 1.0 optional feature, present on every
   Ada part, and it goes in `requirements.cpp` beside the others so a device without it fails naming
   it. One shader, one pipeline, and the "a layout qualifier and a `VkFormat` are one fact written
   twice" hazard `gbuffer.h` describes goes away at that binding rather than being restated.
2. **Two pipelines from one source**, compiled with the binding's format as a `-D`. No new device
   feature, one more pipeline and one more branch at record time.

**Take the first.** It is smaller, it removes a stated hazard, and a required feature this renderer
cannot run without is the answer it gives everywhere else.

**What it buys.** Tap traffic falls 208 more bytes a level — 832 to 624, **40% below today** — and
compulsory traffic 64 to 48. Expect `filter` toward 1.6 ms.

**What it costs.** A filtered bounce quantised to 0.05% and never summed into anything. The
reference path does not run this pass at all.

**Effort**: a day and a half, most of it the pass plumbing.

## 8. Stage 4: one tap for the guide, and measure before building it

`shader-review.md` §9's first bullet: each tap reads a guide and a depth separately, so pack what the
cascade needs of both into one `rgba16f` and a tap becomes one load.

This is the largest remaining win and the only stage with a quality question in it, so it is last.

- **It cannot be `CHANNEL_GUIDE`.** NGX reads that image and wants a three-component normal in it.
  The cascade wants a normal and a distance. Either the trace writes a second, filter-only channel —
  eight more bytes on a pass at 9% of peak, which is affordable — or the trace writes one or the
  other under a specialization constant, since the wavelet and NGX never run in the same frame.
  Prefer the constant: the variant machinery is already there and a channel that is dead in the
  shipping frame is not.
- **The distance is normalised**, per §2.2, and `AtrousConstants` gains the scale.
- **The precision question is real and it is not about long range.** A half's relative step is
  0.049% of the distance; one pixel at 60° over 1080 rows subtends 0.107% of it. Both scale with
  distance, so the quantisation is **0.46 of a pixel footprint at every range**, not just at the far
  end. `mPlaneSigma` is 2 footprints and `offset` is a difference of two quantised positions, so the
  exponent's argument can be off by 0.92 / 2 and a tap's coplanar weight by `exp(0.46)` — **half its
  value**. Averaged over 25 taps much of that cancels; how much is a measurement, not an argument.
- **The normal can be octahedral `rg16_snorm` beside a `rg16f`**, which is §4's own proposal and
  makes a tap 8 bytes. `R16G16_SNORM` is not a required storage format, so it is probed and the
  failure names it. A dozen ALU a corner to decode.

**What it buys, if the quality holds.** Tap traffic 624 to 424 — **59% below today** — and expect
`filter` toward 1.2 ms.

**How to decide it.** `theFilterAndItsHistoryConvergeOnAGrazingSurface` measures exactly this: the
RMS error against a converged reference, with and without the history, on a surface chosen to be
grazing. Build the packed guide, take those two figures, and keep it only if they hold. That is a
half-day experiment in front of a day of work, and the experiment is what says whether to do the
work.

## 9. What does not move, and why

- **`CHANNEL_DIRECT`, `CHANNEL_INDIRECT`** — §2.1.
- **`CHANNEL_MOTION`, `CHANNEL_REFLECTION_MOTION`** — sub-pixel accuracy above 1024 pixels, which is
  where a camera turn puts them. `getMotion` argues it and the DLSS guide permits the halving anyway.
- **`CHANNEL_DEPTH`** — a clip value spends most of its range within a few units of the eye, and the
  distance beside it runs past 65504. Both halves of that image need full floats for different
  reasons.
- **`ACCUMULATE_MOMENTS`** — the cancellation. Its `a` channel is written as zero and read by
  nothing; a three-component 32-bit storage format is not required by Vulkan, so it stays four.
- **`CHANNEL_ALBEDO`, `CHANNEL_SPECULAR`** — already half, measured at 0.0014% against the 0.067%
  the radiance channels were reverted over.
- **`CHANNEL_PARTICLE_MASK`, `CHANNEL_BIAS_MASK`, `CHANNEL_STARS_SHOWN`** — already one, one and four
  bytes.

## 10. Risks

- **The register-allocation knife-edge.** `shader-review.md` records the trace flipping between 96
  and 128 registers on edits that changed no work, and names a narrower G-buffer as a durable fix.
  It is not one: a value written to a half-float image is computed in a full-float register either
  way. Read `Register Count` in the log before reading the timer, and expect the trace zone to move
  for reasons unrelated to these bytes.
- **A format qualifier and a `VkFormat` drifting apart.** The whole class of mistake `gbuffer.h`
  exists to prevent, and every stage here edits both sides. Stage 0 is what keeps each one to a
  single macro.
- **`verify` moves on the wavelet views by design.** Every stage changes what those views compute, so
  a moved view is evidence of nothing on its own. The upscaled views must not move at all, and a
  view that moves there is a real fault.
- **The measurement itself.** The `filter` and `accumulate` zones are 0.5 to 2.8 ms and the box
  throttles: hold a minute of cooling between runs, alternate the binaries, and take the median of
  three. `.notes/rtx/ser-plan.md` §10.1 is the method that worked.

## 11. Acceptance

- Every test under `Rtx*` passes, with the figures in `filter.cpp` re-derived rather than widened.
- `openmw-rtxtool verify` is byte-identical on every upscaled view.
- The `--upscale=off` bench shows `accumulate` and `filter` down, measured the way §10 says.
- The `--upscale=quality` bench is unchanged inside its noise.
- No stage leaves a format stated in two places.

## 12. Sources

- `.notes/rtx/shader-review.md` §4, §9 — the finding this plan is of.
- `components/rtxvulkan/gbuffer.cpp` — the half-float experiment on the radiance channels, and the
  albedo measurement that went the other way.
- `apps/components_tests/rtx/visibility/light.cpp`,
  `aBounceDrawsItsDirectionByTheCosineAndNotUniformly` — the 0.0002 gate on a converged mean.
- `apps/components_tests/rtx/visibility/filter.cpp` — the six tests that measure this cascade.
- DLSS-RR Integration Guide §3.4, in `.refs/dlss/doc` — the input formats NGX takes.
- Dammertz et al. 2010; Schied et al. 2017 (SVGF) — the cascade, and half floats for radiance with
  full floats for moments.
