# RTX shaders: what is still open

A review of every shader and every binding, and an implementation pass over it, left the
items below. Everything else was landed, or was measured and lost, or was withdrawn
against published guidance. What closed each of those is written where the code is, not
here — `lightThrough`, `Hit::mCorner`, `SkyChoice`, `FogRay` and `AtrousPass::sBindings`
each carry the figures that settled them.

## What decides whether an item is worth doing

The GPU zones at the Balmora mages' guild, which is the heaviest place in the suite.

| `--upscale=quality`, the shipping path | ms | `--upscale=off` | ms |
|---|---|---|---|
| upscale | 2.65 | trace | 3.29 |
| trace | 1.44 | filter | 2.07 |
| tlas | 0.19 | accumulate | 0.37 |
| bloom | 0.13 | air | 0.21 |
| air | 0.12 | tlas | 0.19 |
| refit | 0.10 | bloom | 0.14 |
| column, tone, composite, exposure, sprites, skin, shade | 0.06 and under | composite and under | 0.13 and under |

Two things follow. **The à-trous cascade does not run on the shipping path** — Ray
Reconstruction replaces it, and `filter` appears only with the upscaler off. **Nine of the
thirteen zones cost under 0.15 ms**, so an item that halves one of them is worth under a
tenth of a millisecond.

## The rule the measurements produced

**Arithmetic in a shader is free and the path a read takes is not.** Three changes that
each remove real work — an index fetch per hit, six local-array spills, an exponential per
sprite — moved nothing. The one change that moved anything moved a read from the
load-store path to the texture unit, and took 5 to 6 per cent off the cascade. Rank a
candidate by which of the two it is before ranking it by how much work it removes.

## Open

### 1. The fog path still spills local arrays

**Where.** `lib/fog.glsl:34` `FOG_CHURN[FOG_SCALES]`, `:45` `FOG_TURN[FOG_SCALES]`, `:263`
`FogSources::mMoons[2]`, `:340` `fogSourceTermsAlong` returning `vec3[SKY_SOURCES]`, `:362`
and `:363` the `moons` and `weights` pairs.

**Evidence.** `fogscatter.comp.spv` holds five function-scope array variables and
`visibility.rgen.spv` four. `visibilitysurface.rchit.spv` held six and now holds none,
which is what the same treatment did for `gather`.

**What to do.** `SkyChoice` in `lib/lights.glsl` is the pattern: name the three rather than
index them. `FOG_CHURN` and `FOG_TURN` are `const` and indexed by a loop that runs twice,
so unrolling it by hand should fold them away entirely.

**What to expect.** Neutral, by the rule above. Take it for the register file and for the
next reader who adds a fourth scale, not for a number.

### 2. The cone level, split into its two terms

**Where.** `lib/texturing.glsl:85` `coneLod`.

**What.** JCGT 10(1) 2021 section 6 rewrites the level as `½ log2(w·h)` plus a term with no
texture in it. A hit that reads a diffuse map and a glow map, or a chunk of ground that
reads four or five layers, computes the second term once per texture where once per hit
would do.

**What must not be touched.** The `coneWidth <= 0.0` early return. `lightThrough` explains
why: it is what makes a shadow ray's cutout test cheap, and removing it was measured and
lost.

**What to expect.** It is arithmetic, in the trace. Neutral is the likely reading.

### 3. The à-trous tile, and the schedule under it

**Where.** `atrous.comp`, and `AtrousPass::record`.

**What.** One pixel makes 25 taps of three images at each of five levels. An 8x8 group
needs a 12x12 halo at a step of one, 16x16 at two, 24x24 at four — 4800 loads for 432
distinct values at the finest level. A 24x24 tile of nine floats is 21 kB against the 48 kB
the device gives, and covers the three finest levels.

**Where it stops.** The two coarsest levels outgrow any tile and are then half of what is
left. Dolp and others answer that with a permutation before and after each level, so every
level is undilated and all five fit one small tile; they report 1.3x to 3.8x over the whole
filter. That changes which invocation owns which pixel, so it is a second step.

**What to expect.** The pass is 2.07 ms and this is a read-path change, which is the kind
that has paid here. It is worth nothing on the shipping path.

### 4. The motion and upscaled formats

**Where.** `gbuffer.h` `GBUFFER_MOTION`, and `vulkanrenderer.cpp:267` for the upscaled
target.

**What.** Three images carry motion at `rg32f`, which is 24 bytes a pixel. NVIDIA's Ray
Reconstruction guide takes `RG16_FLOAT`. The upscaled target is
`R32G32B32A32_SFLOAT`, which is 133 MB at 3840x2160 that DLSS writes and the tone pass
reads.

**What blocks it.** `lib/reproject.glsl:42` `previousScreen` divides by a depth it only
tests for sign, so a point just in front of the previous camera plane produces an unbounded
motion — large and finite in FP32, infinite in FP16. Clamp that first, on its own
justification. The upscaled target additionally needs the tone pass to read an unformatted
image, because the same binding takes the FP32 composite when the upscaler is off.

**What must not be narrowed.** `GBUFFER_RADIANCE`, and the composite at
`tracechain.cpp:57`. `atrous.h` carries the reason: a reference is accumulated through both
radiance channels, and the composite is the image a measured difference is read from.

### 5. The layer channels, allocated when nothing writes them

**Where.** `GBuffer`'s constructor, against `visibility.rgen:481`.

**What.** `Transparency`, `TransparencyOpacity` and `TransparencyMotion` are written only
where `frame.mLayerCompositedAfter` is set, which is where the frame is upscaled. All three
are always created and always transitioned twice a frame: 20 bytes a pixel, 41 MB at
1920x1080, and six of the 28 G-buffer barriers.

**What to do.** `CompositePass::mNoSum` and `TonePass::mNoBloom` are the pattern. Point the
three at a 1x1 stand-in where the flag is clear.

**What to expect.** Memory and barriers, not time.

### 6. Four library files name the sky and include the frame

**Where.** `lib/sky.glsl` and `lib/starfield.glsl` include `visibility.h` and name only
types that now live in `sky.h`. `lib/frame.glsl` and `lib/variants.glsl` include it and
name nothing from either.

**What to do.** Point each at what it names. It compiles today because `visibility.h`
includes `sky.h`.

**What to expect.** Nothing at runtime. It is the reason `sky.h` was split out, finished.

## Not worth doing, and why

These were on the list and the zone table closed them. They are here so that nobody costs
them out a second time.

- **A shared-memory tile for `fogintegrate.comp`.** Six times fewer fetches of a 0.06 ms
  pass.
- **A subgroup ballot in `spriteruns.comp` and a subgroup scan in `spritestarts.comp`.**
  The whole sprite bin is 0.02 ms.
- **A shared-memory sort in `spriteshade.comp`.** The pass reports 0.00 ms.
- **The branch-free orthonormal basis of Duff and others.** It removes a branch, a cross
  product and a reciprocal square root from every shadow, bounce and ambient ray — which is
  arithmetic, and arithmetic has measured neutral every time here. It also moves every
  stochastic draw in the frame, so it would cost ten pairs to land. Reconsider only if a
  trace profile ever puts `tangentTo` somewhere visible.
