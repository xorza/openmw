# RTX shaders: root causes behind the open items, and what to do about them

Six items were left open after the shader review and the implementation pass. This file
asks what each one is a symptom of, and proposes the fix at that level rather than at the
symptom. Four of the six turn out to share two root causes.

Everything asserted here was checked against the tree or measured on this box. Where a
claim is a measurement, the figures are given. Where it is a hypothesis, it says so.

## Method

Each open item was traced back through its callers until the question stopped being "what
does this line cost" and became "why is the code shaped so that this line exists". Two
probes were run to settle a hypothesis before it became a proposal, and both are reported
below with what they produced.

## What the frame costs, which decides what is worth doing

The GPU zones at the Balmora mages' guild, the heaviest place in the suite.

| `--upscale=quality`, the shipping path | ms | `--upscale=off` | ms |
|---|---|---|---|
| upscale | 2.65 | trace | 3.29 |
| trace | 1.44 | filter | 2.07 |
| tlas | 0.19 | accumulate | 0.37 |
| everything else, each | ≤ 0.13 | air | 0.21 |

**The one rule the earlier measurements produced.** Arithmetic in a shader is free here and
the path a read takes is not: three changes that each removed real work moved nothing, and
the one change that moved a read from the load-store path to the texture unit took 5 to 6
per cent off the cascade. A proposal below that is arithmetic says so and claims nothing.

---

# Root cause A — a type carries what it was made from, not what it is for

`components/rtx/../CLAUDE.md` states the rule this breaks: *"Whatever can be computed once
is computed once. A frame reads what it was handed."* Two types in the shader library carry
their ingredients across every call and let each reader derive the answer again.

## A1. `TexturePoint` and `SurfaceCone` exist to feed one scalar

**What the tree says.** `TexturePoint::mCorner[3]` is read at exactly four lines, all inside
`lib/texturing.glsl`: the three that fill it and the four inside `coneLod` that take a
determinant of it. `SurfaceCone`'s two fields are read at exactly two lines, both inside
`coneLod`. Nothing outside that file has ever wanted either.

**So the shape is.** A hit builds a six-float `TexturePoint` and a two-float `SurfaceCone`,
hands both plus a width to every sampler, and each sampler recomputes one determinant, one
division and three logarithms to reach a scalar that does not depend on which texture is
being read. `resolveFor` does this for the diffuse map, again for the opacity where the
surface is see-through, and again for the glow map. A chunk of ground does it once per
layer, four or five times.

**The fix.** `texturePoint` takes the cone and the width and returns the answer:

```glsl
/// Where a hit lands on a sheet, and how coarse a level the cone can still tell apart there.
struct TexturePoint
{
    vec2 mAt;

    /// The level any texture on this sheet is read at, before its own resolution is added.
    ///
    /// **The answer and not what it was made from.** The three transformed corners and the
    /// surface's own cone were carried to every sampler so that each could take the same
    /// determinant again — `TexturePoint` was six floats wide and `SurfaceCone` two, for
    /// one scalar that is the same for every map on the triangle.
    ///
    /// Minus infinity where the ray carries no cone, which the sampler clamps to the base
    /// level. That is what the two tests in `coneLod` used to return and it is why there
    /// are no tests here.
    float mBase;
};

TexturePoint texturePoint(vec2 uv[3], vec3 weight, vec4 transform, SurfaceCone cone, float coneWidth);

/// That level for one texture, which is the only part its resolution enters.
float coneLod(uint slot, TexturePoint point)
{
    const vec2 size = vec2(textureSize(textures[nonuniformEXT(slot)], 0));

    return point.mBase + 0.5 * log2(size.x * size.y);
}
```

The split is the one Akenine-Möller and others give in *Improved Shader and Texture Level
of Detail Using Ray Cones*, JCGT 10(1) 2021, section 6: the level is one term in the
texture's resolution and one term in nothing else.

**The terrain layers come along for free.** A layer's own transform scales the parcel by
`|sx·sy|`, so its base is the mesh's base plus `0.5·log2(|sx·sy|)` — a scalar add, where
the loop takes a whole determinant a layer today.

**What must survive.** `lightThrough` explains that `coneLod` returning at once for a width
of nought is what makes a shadow ray's cutout test cheap, and that removing that early exit
was measured and lost. It survives here by construction: the width of nought makes `mBase`
minus infinity once, at the one place the point is built, and every sampler after it adds a
finite number to minus infinity and clamps. No test runs on the shadow path at all, where
today one runs per candidate.

**What to expect.** Fewer registers through the call chain, one determinant a hit instead
of two to six, and one fewer branch per candidate on the shadow path. It is arithmetic in
the trace, so the rule above says expect neutral. Take it for the type and measure anyway —
the shadow path loses a branch, not only arithmetic, and that is the one part that could
move.

## A2. The sky sources are an array that nothing treats uniformly

**What the tree says.** Every site that walks `SKY_SOURCES` opens with
`source == SKY_SOURCE_SUN ? A : B`. The sun is not a moon: the surface path draws one ray
between all three, and the fog path always casts the sun's ray and draws only between the
two moons. The array buys a loop whose body branches on its own index.

**What it costs, measured.** `fogscatter.comp.spv` holds five function-scope array
variables and `visibility.rgen.spv` four. A local array with an index the compiler cannot
fold goes to scratch memory on this hardware, which NVIDIA's own shader profiler lists as a
thing to look for. `gather` held six of these and holds none since `SkyChoice` named its
three.

**Probe, run and reverted.** `GL_EXT_control_flow_attributes` and `[[unroll]]` on the two
loops in `lib/fog.glsl` took `visibility.rgen` from four arrays to **none** and
`fogscatter.comp` from five to **three**, with every one of the 23 views drawing an
identical picture. So half of this is a loop the compiler declined to unroll and the tree
never told it to.

**The three that survive a compiler hint**, and why each is a shape rather than a hint:

| Array | Where | Why it stays |
|---|---|---|
| `vec3 terms[SKY_SOURCES]` | `fogSourcesFrom`'s parameter, `lib/fog.glsl:357` | An array crossing a function boundary by value |
| `vec3 moons[2]` | `lib/fog.glsl:362` | Indexed by `moon`, which the draw decides |
| `float weights[2]` | `lib/fog.glsl:363` | The same |

**The fix, in two parts.**

*First, state the intent of every loop over a compile-time shape.* Add
`GL_EXT_control_flow_attributes` and mark the small fixed loops `[[unroll]]`. There are
few: the fog's three scales, the three sky sources, the two wave cascades, the peel's
layers. This is not a hint to hope on — it is the statement that the count is a shape and
not a number, and SPIR-V carries it as `OpLoopMerge Unroll`.

*Second, name the two moons.* `FogSources` carries the drawn source rather than an index
into a pair:

```glsl
struct FogSources
{
    bool mSunlit;
    vec3 mSunward;

    /// Both terms, because a pair not worth a ray is delivered whole.
    vec3 mMasser;
    vec3 mSecunda;

    bool mMoonlit;

    /// The one the draw landed on: what it puts into the air, the source itself so a ray
    /// can be aimed at it, and the chance it was drawn.
    ///
    /// **A `SkySource` and not an index into `frame.mMoons`.** `moonsInAir` read the pair
    /// at a subscript the draw decided and `fogscatter.comp` added that subscript to
    /// `SKY_SOURCE_MASSER` to ask `skyVisible` — two places deriving the same thing from a
    /// number, where the thing itself fits in the struct.
    vec3 mDrawn;
    SkySource mDrawnSky;
    float mChance;
};
```

`fogSourceTermsAlong` then returns a named triple rather than `vec3[SKY_SOURCES]`, and
`fogscatter.comp` calls the `skyVisible(SkySource, …)` overload that `lib/lights.glsl`
already has.

**`SkyChoice` is the precedent and not the thing to share.** The two paths weigh different
quantities — a cosine times an irradiance against a phase term — and draw over different
sets. What they should share is the shape, which is "name them", not a common type.

**What to expect.** Neutral on time, by the rule. Nine spills become none, which is what
the next reader who adds a fourth scale or a third moon inherits.

---

# Root cause B — a value whose failure lives inside its own domain

## B1. `previousScreen` reports "off the left edge" as "no previous frame"

**What the tree says.** `lib/reproject.glsl:42` returns a screen position from nought to one
across, and `vec2(-1.0)` where there is no answer. `reprojected` at `:85` tests
`screen.x < 0.0`.

**The defect.** A surface that reprojects to the left of the previous screen has a
legitimate `screen.x` below nought. The test cannot tell it from the sentinel, so
`reprojected` returns `vec2(0.0)` — *this pixel did not move* — for content that came in
from off-screen. Ray Reconstruction then reuses this pixel's own history for a surface that
was never there. That is the wrong answer, not a missing one, and it fires along the
leading edge of every horizontal pan.

**The second defect, in the same function.** `ahead` is tested for sign and never for size.
A point just in front of the previous camera plane divides by nearly nothing, so the
returned position is unbounded. In FP32 it is a huge finite number that reaches DLSS. It is
also exactly what blocks the motion channels from narrowing to `RG16_FLOAT`, which NVIDIA's
Ray Reconstruction guide accepts and which would take 12 bytes a pixel off the G-buffer.

**The fix.** A result that states its own validity, and a bound:

```glsl
/// Where a point stood on the previous eye's screen, and whether it stood anywhere at all.
///
/// **The two are separate fields because a screen coordinate has no spare value.** A
/// position off the left edge is negative and so was the sentinel, so a surface arriving
/// from off-screen was reported as a surface that had not moved — which is a history reuse
/// and not a history miss.
struct PreviousScreen
{
    /// Nought to one across the previous frame, and outside that where it left the screen.
    /// Bounded: `PREVIOUS_SCREEN_REACH` says how far outside a reprojection may claim.
    vec2 mAt;

    /// False where there is no previous frame, and where the point stood behind that eye.
    bool mFound;
};
```

**What the bound should be, and why it is not arbitrary.** What a consumer needs from a
point that left the screen is the direction and the fact that it left. One screen of margin
on each side carries both, and a motion vector of two screens is already far past anything
an upscaler reuses. Clamping `mAt` to `[-1, 2]` keeps every in-frame value exact, makes the
output bounded by construction, and puts the largest motion at twice the frame width —
which FP16 holds to about one part in two thousand of its own magnitude.

**Then, and only then, the formats.** With the reprojection bounded, `GBUFFER_MOTION` goes
from `rg32f` to `rg16f` across all three motion channels, which is 12 bytes a pixel. The
upscaled target can follow, at 133 MB a frame at 3840×2160, once the tone pass reads it as
an unformatted image — the same binding takes the FP32 composite when the upscaler is off,
and `composite.comp` already uses `GL_EXT_shader_image_load_formatted` for exactly that
reason.

**What must not be narrowed, and this is recorded already.** `GBUFFER_RADIANCE` and the
composite at `tracechain.cpp:57`. `atrous.h` carries the reason: a reference is accumulated
through both radiance channels over hundreds of frames, and the composite is the image a
measured difference is read from. Narrowing the instrument to save bandwidth inverts this
tree's stated order.

**What to expect.** B1 alone is a correctness fix with no time in it. The formats that
follow are 12 bytes a pixel off the G-buffer, and this is a traffic change rather than an
arithmetic one — which is the kind that has paid here.

---

# The three that have no shared cause

## C1. The à-trous cascade reads 375 texels a pixel

**Where it stands.** The sampled-image change already took 5 to 6 per cent off it. What is
left is the schedule: 25 taps of three images at each of five levels, and an 8×8 group
needs a 12×12 halo at a step of one, 16×16 at two and 24×24 at four — 4800 loads for 432
distinct values at the finest level.

**Two steps, and the second is the published one.** A 24×24 tile of nine floats is 21 kB
against the 48 kB the device gives and covers the three finest levels; the two coarsest
outgrow any tile and are then half of what is left. Dolp and others answer that with a
permutation before and after each level so that every level is undilated and all five fit
one small tile, and report 1.3× to 3.8× over the whole filter.

**What it is worth.** The pass is 2.07 ms and it does not run on the shipping path — Ray
Reconstruction replaces it, and `filter` appears in the zone table only with the upscaler
off. It is worth the length of a reference run and nothing else.

## C2. The layer channels are allocated where nothing writes them

**Root cause.** `sEveryChannel` is a compile-time list with no notion of a channel that a
frame may or may not carry, so `GBuffer` creates all fourteen and transitions all fourteen
twice a frame whatever the frame does.

**What it costs.** `Transparency`, `TransparencyOpacity` and `TransparencyMotion` are
written only under `frame.mLayerCompositedAfter`, which is set only when the frame is
upscaled — `visibility.rgen:481`. Twenty bytes a pixel, 41 MB at 1920×1080, and six of the
28 G-buffer barriers.

**The fix, and it is already in the tree twice.** `CompositePass::mNoSum` and
`TonePass::mNoBloom` point an unused binding at a 1×1 stand-in. `GBuffer` takes a flag and
does the same for the three. The set layout keeps its fourteen bindings, so no shader
changes, and an out-of-range `imageStore` is discarded by the specification rather than
undefined.

**What to expect.** Memory and barriers. No time.

## C3. Four library files name the sky and include the frame

`lib/sky.glsl` and `lib/starfield.glsl` include `visibility.h` and name only types that now
live in `sky.h`. `lib/frame.glsl` and `lib/variants.glsl` include it and name nothing from
either. It compiles because `visibility.h` includes `sky.h`. Point each at what it names.

---

# Implementation plan

Seven steps. Each is one commit. The gate for every step is the same chain, and no step
begins before the one before it is green:

```
ninja -C build-debug && ninja -C build-release
./build-debug/components-tests --gtest_filter='Rtx*'
./build-debug/openmw-rtxtool verify --against <the previous step's output>
./build-debug/openmw-rtxtool check
CLANG_FORMAT=clang-format-14 CI/check_clang_format.sh
apps/rtxtool/repeatable.sh --pairs=10
```

A step whose gate is a picture also runs
`./build-debug/openmw-rtxtool shot --view=<place> --sync-validation`.

### Step 1 — C3, the includes

Point `lib/sky.glsl`, `lib/starfield.glsl`, `lib/frame.glsl` and `lib/variants.glsl` at
what they name.

**Gate.** Identical opcode stream in every module. Nothing may move.

### Step 2 — A2 first part, the unroll attribute

Add `GL_EXT_control_flow_attributes` and mark every loop over a compile-time shape
`[[unroll]]`: the fog's scales and sources, the wave cascades, the peel's layers.

**Measured already.** Nine function-scope arrays become three, and all 23 views draw an
identical picture. The probe was run and reverted.

**Gate.** Bit-identical picture. Count the arrays before and after with
`spirv-dis … | grep -c 'OpVariable %_ptr_Function__arr'` and record both numbers.

### Step 3 — A2 second part, the named moons

`FogSources` carries the drawn source. `fogSourceTermsAlong` returns a named triple.
`fogscatter.comp` calls the `SkySource` overload of `skyVisible`.

**Gate.** Bit-identical picture, and zero function-scope arrays in `fogscatter.comp.spv`.
The draw must keep its place in the sequence, which is the one way this moves the picture
by accident.

### Step 4 — B1, the reprojection

`PreviousScreen` with its own validity and a bound. Every caller of `previousScreen` takes
the struct: `reprojected` in `lib/reproject.glsl` and `fogVolumeWas` in `fogscatter.comp`.

**Gate.** The picture *will* move, along the leading edge of a pan, and it moves toward the
right answer. Compare a still with `--exposure=1`, and confirm the change is confined to
pixels whose motion points off-screen. Ten pairs. `verify` over 23 views is a still
camera, so most views should be untouched — a view that moves and is not
`one-cell-walk` wants explaining.

### Step 5 — A1, the cone level

`TexturePoint` carries `mBase`. `coneLod` adds the texture's resolution and nothing else.
The terrain loop adds its layer's scale factor rather than taking a determinant.

Write the test first: a case that asks for a level with a width of nought and asserts the
sampled texel is the level-zero texel. That is the one behaviour the branch removal has to
preserve, and it is the behaviour `lightThrough` depends on.

**Gate.** Bit-identical picture. Then measure the trace at the Balmora mages' guild and at
Seyda Neen's shore, three interleaved pairs each, and record the number whichever way it
falls. The shadow path loses a branch per candidate, which is the one part that could show.

### Step 6 — B1's formats

`GBUFFER_MOTION` to `rg16f`. Then, separately, the upscaled target to
`R16G16B16A16_SFLOAT` with the tone pass reading it unformatted.

**Gate.** Two commits, two readings. Take each with `--exposure=1` and read the floats. Ten
pairs each. State the bytes saved per pixel beside the measured frame time.

### Step 7 — C2, the stand-in channels

`GBuffer` takes a flag for the layer channels.

**Gate.** Nothing may change with DLSS on, and nothing may change with it off. Report the
device memory before and after.

### Left out of the plan

**C1, the à-trous tile and the permutation.** It is the largest remaining number and it is
on a path the frame budget is not written against. Take it when a reference run's length
starts to cost something, and take the tile before the permutation.

---

# What this plan is worth, honestly

| Step | Kind | Expected |
|---|---|---|
| 1 | hygiene | nothing at runtime |
| 2 | spills | neutral on time, nine scratch arrays gone, measured |
| 3 | spills | neutral on time, the last three gone |
| 4 | **correctness** | a wrong motion vector along every pan edge, fixed |
| 5 | arithmetic and one branch | neutral is the likely reading |
| 6 | **traffic** | 12 bytes a pixel, then 133 MB a frame at 4K |
| 7 | memory | 41 MB and six barriers |

Two of the seven are worth taking for a number: step 6, because it is traffic and traffic
is what has paid here, and step 4, because it is not a performance change at all. The rest
are the type and the shape, and the honest expectation for every one of them is that the
clock does not move.
