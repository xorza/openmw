# Translucency, sprites and media: what is wrong and what to build

Three faults reported against the picture turned out to be one structure. This is what they are, what the
content actually holds, what the field does about it, and the order I would build the answer in.

Nothing here is implemented. The two reproductions below are the acceptance tests.

## The three symptoms

**A blight cloud drawn as a solid red sheet.** Dagoth Ur, cell `2,8`.

```
openmw-rtxtool shot --cell=2,8 --pos=19782,65836,16350 --look=20000,66700,15900 --size=960x540
```

**A drain splash drawn as a dark blob against fog.** Vivec, cell `2,-10`.

```
openmw-rtxtool shot --cell=2,-10 --pos=19161,-79415,1467 --look=19643,-80010,824 \
    --hour=8 --weather=Cloudy --size=960x540
```

**A pane taking the haze of a path it is only part of the way along.** Not reported by anyone; stated and
accepted at `visibility.rgen:156`.

## The root

Four media stand between the eye and a surface — the peeled pane, the water column, the air, and the
sprites. Each is applied as **one multiply over the whole path**, in a fixed order, at the end of the ray.
None of them knows where the others sit along it.

```glsl
shaded = shaded * paneThrough + paneRadiance;                          // over the whole path
shaded = throughWater(shaded, column);                                 // over the whole path
shaded = shaded * fog.w + fog.xyz;                                     // over the whole path
shaded = shaded * particles.mTransmittance + particles.mRadiance;      // over the whole path
```

Every one of the three symptoms falls out of that line-up.

- **The sprite** dims the fog's in-scattering, the part in front of the sprite included. `visibility.rgen`
  says so and rests on an assumption — "which is the part of the ray a puff of smoke is nearly always at
  the end of". At Vivec the splash is at the *near* end and the haze behind it is thick and bright, so the
  sprite subtracts most of that brightness and returns only its own small radiance. That is the dark blob.
- **The pane** is measured over the distance to the surface behind it rather than over its own stretch.
- **The blight cloud** is not a compositing error at all but a *layer count*: the eye peels one translucent
  surface and paints everything behind it as though opaque.

## What the content actually holds

`meshes/f/active_blight_large.nif`, at `20864, 68960, 10816`:

| | |
|---|---|
| `NiAlphaProperty` | 11 |
| `NiMaterialProperty` | 12 |
| `NiAlphaController` | 10 |
| textures | `Tx_Dagoth_Cloud.tga`, `Tx_Dagoth_cloud02.tga` |

Eleven alpha shells, doubled for their backs by `ShapeFold`. Both textures are **DXT3** — explicit graded
alpha, which is what an author reaches for when the alpha is not a mask. Decoded:

| texture | size | clear | solid | partial | peak |
|---|---|---|---|---|---|
| `tx_dagoth_cloud.dds` | 128×128 | 28.2% | **0.0%** | 71.8% | 7/15 |
| `tx_dagoth_cloud02.dds` | 128×64 | 44.1% | **0.0%** | 55.9% | 10/15 |

**No texel in either is ever solid.** A surface that is nowhere opaque is not a surface.

**The classification is already right and is not the bug.** Traced over the whole region, the cloud's
materials arrive translucent with a material alpha between 0.045 and 0.4, so the ten `NiAlphaController`s
run and `Surface::Material` follows them. Exactly ten blend surfaces in the whole of Red Mountain are
treated as cutouts, and every one is foliage — leaves, vines, branches, thatch, rope. `isTranslucent`
separates a leaf from a pane correctly.

**What is wrong is the census the peel was sized against.** `visibility.rgen`:

> **The nearest one only.** … For glass that is nearly always the whole answer — the census says a cell
> holds two to four translucent materials.

`scene --cell=2,8` reports **205 translucent materials**, and one mesh carries eleven shells.

## What the field does, and what it costs

**Re-trace per layer** — what "peel more" becomes. Measured at **9.6–12.1 ms** in Sponza with early-out at
`T < 0.05`, against 1.2 ms for raster alpha blending [1]. Correct and ordered, and far outside the budget.

**Any-hit transmittance in one traversal.** Cheap, and right for transmittance alone: the any-hit meets
layers in arbitrary order, so it cannot composite colour. This is how the shadow ray already works, and it
is why it works there and not for the eye.

**Stochastic single-shade.** Shade one fragment per pixel, chosen against a mask that is blue noise in
space *and* time, read alpha as coverage, lean on hardware early termination and a temporal resolve [2].
Cost is one traversal. This renderer already has blue noise per stream, a per-frame jitter, and DLSS doing
the resolve.

**Thickness rather than coverage.** Spherical billboards measure how much medium a ray crosses and use
`1 - exp(-density · thickness)` [3]. `rtxmw/docs/design.md` §8.82 reached the same conclusion for the moons
behind cloud, in its own words: *"A cloud is a depth, not a coverage."*

Ray Tracing Gems II chapter 11 is the other canonical source and is behind a login.

## The input we are not filling

Ray Reconstruction takes a particle layer of its own. From `nvsdk_ngx_helpers_dlssd_vk.h`:

```c
NVSDK_NGX_Resource_VK* pInTransparencyLayer;        /* optional input res particle layer */
NVSDK_NGX_Resource_VK* pInTransparencyLayerOpacity; /* optional input res particle opacity layer */
NVSDK_NGX_Resource_VK* pInTransparencyLayerMvecs;   /* optional input res transparency layer mvecs */
```

`DlssPass` fills `pInIsParticleMask` and none of these. So the transparency is composited into the colour
and the denoiser is then told to distrust those pixels rather than being handed the layer itself.

`visibility.rgen` already names the harm and calls it unanswered:

> Its motion vector, its depth and the guide the upscaler demodulates by all stay the surface behind it …
> `SpriteClaim` is what answering that properly looks like.

`SpriteClaim` exists and already picks the owning sprite's motion. That is the vector
`pInTransparencyLayerMvecs` wants.

## The plan

### 1. Split the air at each layer's depth

**What it fixes.** Vivec, and the pane's over-darkening with it.

`fogAlong(pixel, origin, direction, distance, offset, seed)` already takes a distance, so the air can be
measured over any stretch. Ask it for the air *in front of* a layer, composite the layer against that, and
carry on. Each medium then dims only what is behind it, which is the whole of two of the three symptoms.

**Touches.** `visibility.rgen`, and the sprite layer's return so it can say at what distance its coverage
sits. `SpriteLayer` already tracks the covering sprite's claim; the depth is the same walk.

**Checked by.** The Vivec shot above. The splash stops being darker than the haze it stands in.

### 2. Give Ray Reconstruction its particle layer

**What it fixes.** The temporal behaviour of every translucent thing, without touching light transport.

Write the transparency to its own target rather than into the colour, with its opacity and with
`SpriteClaim`'s motion, and pass all three to `NGX_VULKAN_EVALUATE_DLSSD_EXT`. The base colour then carries
the surface behind, which is what the depth and the demodulation guide already describe.

**Touches.** `DlssPass`, the channel set, and the end of `visibility.rgen`.

**Checked by.** `bench --views=balmora-storm-night` and a window: a rainstorm's drops stop smearing, and the
frame behind them stops being resolved as though the drops were part of it.

### 3. Make the shells a medium

**What it fixes.** Dagoth Ur.

One traversal collects every translucent crossing along the eye's ray. `candidateStops` already walks past
with `seeThrough = true` and accumulates through — the shadow ray uses that path today. Turn each crossing
into a thickness rather than a coverage, shade it with the puff light rather than a surface shade, and
composite `1 - exp(-density · thickness)` in order. The blight cloud is then a dozen thin puffs, and the
peel survives only for a true pane, which is what it was built for.

**Touches.** `visibility.rgen`, `traversal.glsl`, and a way for a material to say it is a medium. The
measurement that says so is already made above and is a property of the texture alone: **a diffuse alpha
that never reaches solid is a wisp, not a mask.**

**Checked by.** The Dagoth Ur shot above, and the `bench` suite for what a caldera full of shells costs.

### 4. Dither, only if 3 is noisy or slow

Shade one layer per pixel against the blue noise and let the denoiser resolve it [2]. Step 2 is what makes
this safe: the denoiser has to know the layer is its own before it can resolve noise in it.

## Measure before step 3

**How many translucent crossings a ray makes at Dagoth Ur.** That number is what the march costs, and it
decides whether step 4 is needed at all. The harness can report it beside the hit fraction.

Steps 1 and 2 are worth doing whatever is decided about 3.

## Sources

1. Kostas Anagnostou, *Raytraced Order Independent Transparency*.
   https://interplayoflight.wordpress.com/2023/07/15/raytraced-order-independent-transparency/
2. Brüll, Kern and Grosch, *Spatio-Temporal Dithering for Order-Independent Transparency on Ray Tracing
   Hardware*, EGSR 2025. https://github.com/TU-Clausthal-Rendering/SpatioTemporalDithering
3. Umenhoffer and Szirmay-Kalos, *Spherical billboards and their application to rendering explosions*.
   https://www.semanticscholar.org/paper/7a249dbd873f02248eecc3ebf811b71ea5e90277
4. NVIDIA, *RTX Remix advanced path-traced particle system*.
   https://www.nvidia.com/en-us/geforce/news/rtx-remix-advanced-particle-vfx/
