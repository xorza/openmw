# RTX shaders: fix proposal and implementation plan

This file follows `.notes/rtx-shader-review.md`. The review listed what is wrong. This one
says what to write, in what order, and what has to be true before each step is done.

Every proposal below was checked against published guidance. Section 1 lists the sources.
Section 2 lists what the research changed in the review. Section 3 holds the fixes.
Section 4 is the plan. Section 5 is the risk register.

---

## 1. Sources consulted

| Source | What it settles here |
|--------|----------------------|
| NVIDIA, *Best Practices for Using NVIDIA RTX Ray Tracing (Updated)* | Payload size, hit records, any-hit shaders, barrier batching, workgroup shape |
| Akenine-Möller, Nilsson, Andersson, Barré-Brisebois, Toth, Karras, *Improved Shader and Texture Level of Detail Using Ray Cones*, JCGT 10(1), 2021 | Mip level 0 is always slower than a cone level. How to split the level equation |
| Akenine-Möller and others, *Texture Level of Detail Strategies for Real-Time Ray Tracing*, Ray Tracing Gems ch. 20 | Ray cones against level 0, and cones on secondary rays |
| Duff, Burgess, Christensen, Hery, Kensler, Liani, Villemin, *Building an Orthonormal Basis, Revisited*, JCGT 6(1), 2017 | The branch-free basis, and its measured cost |
| Khronos Vulkan Samples, *Using pipeline barriers efficiently* | One call with all barriers. Conservative stages flush the pipeline |
| AMD GPUOpen, *Vulkan Barriers Explained* | `ALL_COMMANDS_BIT` forces a flush |
| AMD GPUOpen, *Fast Compaction with mbcnt* | Ballot beats a shared atomic inside one wave |
| Khronos, *Vulkan Subgroup Tutorial* | Subgroup traffic costs about what ALU costs |
| AMD FidelityFX Denoiser 1.3 manual | Small-radius filter passes cache their taps in group-shared memory |
| Dolp and others, *A Fast GPU Schedule for À-Trous Wavelet-Based Denoisers* | A permutation makes every wavelet level undilated. 1.3x to 3.8x |
| NVIDIA Streamline, *ProgrammingGuideDLSS_RR.md* | The format each Ray Reconstruction input accepts |

Two quotations carry more weight than the rest.

> "despite the extra computations needed by ray cones (both isotropic and anisotropic),
> using mipmap level 0 is always slower. This is due to the reduction in memory bandwidth
> usage of ray cones, due to better texture cache locality, which turns into a performance
> benefit." — JCGT 10(1), 2021, section 5.

> "Keep the ray payload small. Registers are used to hold payload values and they reduce
> the number of registers otherwise available to hit shaders." — NVIDIA RTX best
> practices.

---

## 2. What the research changed

Three items in the review move. Two of them move against my own proposal.

### F11 is withdrawn. Keep the fifteen hit records.

The review proposed one `uint mLayer` in the payload in place of five shader records per
hit shader. NVIDIA's guidance says the opposite twice. It says to keep the payload small
because payload words take registers away from hit shaders. It also says that values
stored directly in the hit records are an efficient way to give a hit shader what it needs.
The tree already does the endorsed thing. **Do not make this change.**

### F30 is withdrawn. Keep the two masks separate.

`dlsspass.cpp:172` sets `pInIsParticleMask` and `pInBiasCurrentColorMask`. These are two
NGX parameters and each takes its own image. They cannot share one.

### F18 is folded into F3 and loses its own step.

No source gives a measured figure for `imageLoad` against `texelFetch` on this hardware.
The ray-cones paper gives the nearest evidence: a read that lands in the texture cache is
faster than one that does not, and that is why level 0 loses. That is an argument for the
texture unit, not a measurement of it. Change the à-trous reads with F3 and measure the
pair once.

### F1 gains a citation and moves to the front.

The ray-cones paper states that level 0 is always slower than a cone level, and gives the
reason: texture cache locality. `lightThrough` asks for level 0 on every shadow ray.

---

## 3. The fixes

Each fix gives the change, a sketch in the tree's own style, the source behind it, and the
gate that says it worked.

### Fix A. Give a shadow ray the cone it already has

**Files.** `lib/traversal.glsl`, `lib/lights.glsl`, `lib/shading.glsl`, `lib/water.glsl`,
`lib/hitstage.glsl`, `fogscatter.comp`.

**The change.** `lightThrough` takes a footprint and hands it to `RTX_RESOLVE`. Every
caller has one.

```glsl
/// @param footprint how wide the asking ray's cone was where it started, which is what a
///        cutout on the way is tested at. Nought reads the finest level of every mask it
///        crosses, which costs bandwidth and buys nothing: JCGT 10(1) 2021 measures level
///        zero slower than a cone level in every scene it tries, for that reason alone.
float lightThrough(vec3 from, vec3 towards, float distance, float footprint)
{
    if (distance <= SHADOW_BIAS)
        return 1.0;

    float through = 1.0;
    uint crossed = 0u;

    rayQueryEXT query;
    rayQueryInitializeEXT(
        query, sceneTop, gl_RayFlagsTerminateOnFirstHitEXT, MASK_SOLID, from, SHADOW_BIAS, towards, distance);
    RTX_RESOLVE(query, towards, footprint, through, crossed, true)

    if (rayQueryGetIntersectionTypeEXT(query, true) != gl_RayQueryCommittedIntersectionNoneEXT)
        return 0.0;

    return through;
}
```

**The callers.**

| Caller | Footprint to pass |
|--------|-------------------|
| `skyVisible` in `lights.glsl:48` | a new parameter, from `gather`'s own `footprint` |
| `lampVisible` in `lights.glsl:373` | a new parameter, carried down from `lampsThrough` |
| `ambientReaching` in `shading.glsl:319` | a new parameter, the caller's surface footprint |
| `fogscatter.comp:242` | `froxelStrideAt(along)`, which is the froxel's own width |

**Do it in two steps.** Step A1 passes the start width and leaves the spread at nought.
That alone moves every cutout from level 0 to the level the shading point itself reads.
Step A2 adds the source's own angular size as a spread, which is the physically right
answer for a penumbra and is what the ray-cones paper does for secondary rays with a fixed
cone angle. Measure A1 before you write A2.

**Gate.** The scene columns must repeat exactly. The picture moves where a coarse mip
softens a cutout in shadow, which is the correct answer. Take ten pairs.

### Fix B. One sky source per source

**Files.** `lib/lights.glsl`, `lib/shading.glsl`, `fogscatter.comp`.

**The change.** `gather` builds `SkySource` five times for three sources, keeps two
`float[3]` arrays and indexes them with a value the compiler cannot fold. The compiled
module shows six `float[3]` variables in the function storage class of
`visibilitysurface.rchit.spv`, which is the signature of a spill.

```glsl
/// One sky source, weighed for the point that is about to draw one of them.
struct SkyChoice
{
    SkySource mSky;

    /// What the surface makes of its direction, which is nought where it is not asked.
    float mCosine;

    /// What it would deliver unshadowed, as a luminance.
    float mWeight;
};

SkyChoice skyChoiceAt(uint source, vec3 normal, vec3 side, float transmission, bool asked)
{
    const SkySource sky = skySourceAt(source);
    const float cosine = asked ? litCosine(normal, side, sky.mDirection, transmission) : 0.0;

    return SkyChoice(sky, cosine, cosine > 0.0 ? cosine * dot(sky.mIrradiance, LUMINANCE_WEIGHTS) : 0.0);
}
```

`gather` then names its three, in the order the draw already walks:

```glsl
    const SkyChoice sun = skyChoiceAt(SKY_SOURCE_SUN, normal, side, transmission, sunUp());
    const bool lunar = HAS_MOONS && path == PATH_SEEN;
    const SkyChoice masser = skyChoiceAt(SKY_SOURCE_MASSER, normal, side, transmission, lunar);
    const SkyChoice secunda = skyChoiceAt(SKY_SOURCE_SECUNDA, normal, side, transmission, lunar);

    const float total = sun.mWeight + masser.mWeight + secunda.mWeight;
    if (total > 0.0)
    {
        // The same two additions in the same order the loop made, so the pick is the pick
        // it was: a running share against the draw, and not a weight against a scaled one.
        const float sunShare = sun.mWeight / total;
        const float moonShare = sunShare + masser.mWeight / total;

        SkyChoice picked = secunda;
        if (moonDraw[1].x < sunShare)
            picked = sun;
        else if (moonDraw[1].x < moonShare)
            picked = masser;

        radiance += picked.mSky.mIrradiance
            * lightThroughWater(position, picked.mSky.mDirection, footprint)
            * (picked.mCosine * INV_PI * skyVisible(picked.mSky, position, sunDraw, footprint)
                / (picked.mWeight / total));
    }
```

`skyVisible` takes the source rather than an index:

```glsl
float skyVisible(SkySource sky, vec3 position, vec2 draw, float footprint)
{
    return lightThrough(position, coneDirection(sky.mDirection, sky.mLimb, draw), frame.mFar, footprint)
        * cloudShadow(position, sky.mDirection);
}
```

**Why this is exact.** Every addition, division and comparison happens in the order the
loop made them. Nothing is reassociated. The frame hash must not move, and that is the
test.

**What it removes.** Two `float[3]` locals, three `skySourceAt` calls, one `break`, and
one dynamic index. It also gives Fix A the place to pass the footprint.

**Gate.** The picture must be bit-identical. If it is not, the rewrite reassociated
something. Find it.

### Fix C. Split the cone level into the part that depends on the texture and the part that does not

**Files.** `lib/texturing.glsl`, `lib/traversal.glsl`, `lib/medium.glsl`.

**The source.** JCGT 10(1) 2021, section 6, rewrites the level equation as

```
Δ = ½ log2(w·h)  +  ½ log2(t_t / p_a)
    texture dependent    texture independent
```

and says the second term is computed once for a hit that reads more than one map.

**The change.**

```glsl
/// What a cone resolves at this hit, before any one texture's resolution is applied.
///
/// **Split for the reason JCGT 10(1) 2021 section 6 gives.** A hit reads a diffuse map, a
/// glow map and, on ground, four or five layers. Only the first term below differs between
/// them, so the rest is taken once.
///
/// **No test for a degenerate cone, and none is needed.** A width of nought or an area of
/// nought sends the logarithm to minus infinity, and the sampler clamps a level below the
/// base to the base — which is the answer the two tests used to return. The guard on the
/// surface area is the one case that would otherwise divide nought by nought.
float coneBase(TexturePoint point, SurfaceCone cone, float coneWidth)
{
    const vec2 uv0 = point.mCorner[0];
    const vec2 uv1 = point.mCorner[1];
    const vec2 uv2 = point.mCorner[2];
    const float uvArea = abs((uv1.x - uv0.x) * (uv2.y - uv0.y) - (uv2.x - uv0.x) * (uv1.y - uv0.y));

    return 0.5 * log2(uvArea / max(cone.mArea, 1.0e-30)) + log2(coneWidth) - log2(cone.mFacing);
}

float coneLod(uint slot, float base)
{
    const vec2 size = vec2(textureSize(textures[nonuniformEXT(slot)], 0));

    return base + 0.5 * log2(size.x * size.y);
}

vec4 sampleDiffuse(uint slot, TexturePoint point, float base)
{
    return textureLod(textures[nonuniformEXT(slot)], point.mAt, coneLod(slot, base));
}
```

**What it removes.** Two branches from the hottest sampler in the tree. One determinant
and three logarithms per extra texture on a hit. For a chunk of ground with five layers
that is four determinants and twelve logarithms saved.

**Watch this.** `coneWidth` of nought now reaches `log2` and gives minus infinity. Confirm
that the driver clamps rather than producing a NaN. Write the test first:
a `Rtx*Cone*` case that asks for a level with a zero width and asserts the sampled texel is
the level-zero texel.

**Gate.** Bit-identical picture, once Fix A has already moved the shadow rays. Do C after
A, not before, or the two changes mix.

### Fix D. The branch-free basis

**Files.** a new `lib/basis.glsl`, `lib/random.glsl`, `spriteshade.comp`.

**The source.** Duff and others, JCGT 6(1) 2017, listing 3. Their table 1 puts the
branch-free form at 9.35 ns against Hughes-Möller's 18.59 ns, with an RMS error three
orders of magnitude better than Frisvad's. `tangentTo` is a Hughes-Möller: pick a helper,
cross, normalize.

**The change.** A new file with no bindings in it, so a standalone compute shader can read
it too.

```glsl
/// Two unit vectors square to `axis` and to each other.
///
/// Duff and others, "Building an Orthonormal Basis, Revisited", JCGT 6(1), 2017, listing 3.
/// The sign of `z` rides as a multiplier rather than as a branch, so there is no test, no
/// cross product and no reciprocal square root — the tangent this replaces paid all three,
/// and every shadow ray, bounce and ambient ray in the frame draws through one.
struct Basis
{
    vec3 mAcross;
    vec3 mUp;
};

Basis basisAbout(vec3 axis)
{
    const float side = axis.z >= 0.0 ? 1.0 : -1.0;
    const float a = -1.0 / (side + axis.z);
    const float b = axis.x * axis.y * a;

    return Basis(vec3(1.0 + side * axis.x * axis.x * a, side * b, -side * axis.x),
        vec3(b, side + axis.y * axis.y * a, -axis.y));
}

/// A direction with `local.xy` across that basis and `local.z` along the axis.
vec3 alignedTo(vec3 axis, vec3 local)
{
    const Basis basis = basisAbout(axis);

    return basis.mAcross * local.x + basis.mUp * local.y + axis * local.z;
}
```

`coneDirection` and `cosineDirection` then differ only in how they draw the local
direction, which is what they always did:

```glsl
vec3 coneDirection(vec3 axis, float sine, vec2 u)
{
    const float cosine = sqrt(max(1.0 - sine * sine, 0.0));
    const float versine = sine * sine / (1.0 + cosine);

    const float drop = u.x * versine;
    const float radius = sqrt(drop * (2.0 - drop));
    const float turn = TAU * u.y;

    return alignedTo(axis, vec3(radius * cos(turn), radius * sin(turn), 1.0 - drop));
}

vec3 cosineDirection(vec3 normal, vec2 u)
{
    const float radius = sqrt(u.x);
    const float angle = TAU * u.y;

    return alignedTo(normal, vec3(radius * cos(angle), radius * sin(angle), sqrt(max(1.0 - u.x, 0.0))));
}
```

`spriteshade.comp` drops `squareTo` and takes `basisAbout(toward)` for both of its axes.

**Gate.** Every drawn direction changes, so every stochastic term moves. The estimator
stays unbiased and the frame settles to the same answer. This needs its own reading over
ten pairs, and a look at a still with `--exposure=1`. Land it alone.

### Fix E. Resolve the triangle once

**Files.** `lib/traversal.glsl`.

**The change.** `Hit` carries the corner indices that `committedHit` already resolved.
`mPrimitive` goes, because `traversal.glsl:581` was its only reader.

```glsl
struct Hit
{
    bool mHit;
    uint mInstance;

    /// Where in the shared buffers this triangle's three vertices are.
    ///
    /// **Resolved inside the query and carried out.** The index block had to be read there
    /// anyway, to interpolate the shading normal while the object-to-world matrix was still
    /// in scope. `resolveFor` read it a second time for the same three numbers.
    uvec3 mCorner;

    vec2 mBary;
    float mDistance;
    float mFootprint;
    vec3 mCrossed;
    vec3 mShading;
};
```

`committedHit` fills `mCorner` where it already resolved it. `resolveFor` drops its own
`triangleCorners` call and takes `hit.mCorner`.

**What it costs.** `Hit` grows by two words: three for the corners against one for the
primitive. In a ray-query path that is registers, and registers are what the trace is
short of. Measure it.

**What it saves.** One block-table address load and three index loads per hit, on every
primary ray, every bounce, every water ray and every reflection.

**Gate.** Bit-identical picture. If the trace time goes up, the register cost beat the
load saving, and the change should be dropped rather than argued about.

### Fix F. Hoist what a fog column does not learn from its span

**Files.** `lib/fog.glsl`, `lib/sprites.glsl`, `lib/medium.glsl`.

**The change.** `fogColumn(origin, direction, span)` computes `exp(-enters / scale)` from
terms that do not contain `span`. `spritesAlong` calls it once per sprite down one ray.

```glsl
/// What a column of air along one ray is, before anything says how far to follow it.
struct FogColumn
{
    /// How far the eye stands over the fog's base, which may be under it.
    float mFrom;

    float mSlope;
    float mScale;

    /// `exp(-max(mFrom, 0) / mScale)`, which every span from this origin shares.
    float mEntering;

    /// One where the air reaches the ground, nought where a sea floor cuts it off.
    float mUnder;
};

FogColumn fogColumnFrom(vec3 origin, vec3 direction)
{
    const float scale = FOG_HEIGHT * frame.mFogLift;
    const float from = origin.z - fogBase();

    return FogColumn(from, direction.z, scale, from > 0.0 ? exp(-from / scale) : 1.0, fogPools() ? 0.0 : 1.0);
}

float fogColumnOver(FogColumn column, float span);
```

`spritesAlong` builds the column once before the sprite loop. `mediumAlong` and
`fogAlong` build one and use it once, which reads the same as before.

**Why it is exact.** `enters` is `mFrom` in both cases where `mFrom > 0`, and nought
otherwise, so `mEntering` is the number the old code computed. Nothing is reassociated.

**Gate.** Bit-identical picture. Measure at Balmora at night in the rain.

### Fix G. Send the constants the shader asks the driver for

**Files.** `rtx/shaders/visibility.h`, `lib/sea.glsl`, `lib/sprites.glsl`, `lib/fog.glsl`,
`fogscatter.comp`, `visibilitypass.cpp`.

**The change.** Three sites query a texture the host already has the size of.

`waveLevel` asks for the grid of a wave tile. `sWaveTiles` states it as 512 and 128 at
compile time. Add a field beside the extent the pass already writes:

```cpp
        /// How wide one texel of each cascade is, in world units.
        ///
        /// **Written by the pass beside `mWaveExtent`, for the same reason.** A shader that
        /// asked the driver instead spent a texture-header read for a compile-time constant,
        /// twice a cascade, in a function an underwater pixel calls six times.
        float mWaveTexel[WAVE_CASCADES];
```

```glsl
float waveLevel(uint cascade, float footprint)
{
    return max(log2(footprint / frame.mWaveTexel[cascade]), 0.0);
}
```

`puffLight` and `fogVolumeAlong` and `fogscatter.comp` all ask for the froxel grid.
`FogVolume` holds `mColumns` and `mRows`. Add one field and drop three queries:

```cpp
        /// How many columns and rows of froxels stand in front of the camera.
        uvec2 mFogColumns;
```

**What it costs the block.** `VisibilityConstants` grows sixteen bytes. Both
`static_assert`s move: `offsetof(mTables)` from 992 to 1008 and `sizeof` from 1112 to
1128. Confirm the new numbers rather than guess them.

**Gate.** Bit-identical picture. `RtxSceneDigest` and the frame hash both cover this.

### Fix H. One barrier command for a run of images

**Files.** `fogvolume.cpp`, `bloompass.cpp`, `accumulatepass.cpp`, `wavepass.cpp`.

**The source.** The Khronos pipeline-barriers sample says multiple `vkCmdPipelineBarrier`
calls are bad practice and one call with all barriers is good practice, and measures 13 per
cent of a frame on the case it studies. NVIDIA's ray-tracing guidance says the same about
build calls. `image.hpp:162` already holds the class, and `GBuffer` already uses it.

**The change.** Every loop of the shape

```cpp
        for (const Image* image : { &mScatter[written], &mSunward[written], /* ... */ })
            image->transition(commands, /* ... */);
```

becomes

```cpp
        Barriers barriers(commands);
        for (const Image* image : { &mScatter[written], &mSunward[written], /* ... */ })
            barriers.add(image->describeTransition(/* ... */));

        barriers.flush();
```

`FogVolume::begin` holds eleven images in two groups with different destination accesses.
Both groups go into one `Barriers`, because the class takes a whole barrier and not a
shared pair of masks.

**Then, and separately, narrow the stages.** `FogVolume::begin` and `GBuffer::begin` name
`VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT` with `MEMORY_READ | MEMORY_WRITE` as the source.
That is a full pipeline flush twice a frame. The real producer is the previous frame's
trace, its compute passes and, for the channels DLSS reads, the upscaler. Narrowing needs
the frame graph in front of you and is a separate step.

**Gate.** No picture change at all, and the validation layers stay quiet. Run the
asan build, which is where a synchronization mistake shows.

### Fix I. A tile in shared memory for the two filters

**Files.** `atrous.comp`, `fogintegrate.comp`, and the two passes that dispatch them.

**The source.** The FidelityFX denoiser manual says its small-radius passes cache their
taps in group-shared memory for exactly this reason. Dolp and others report 1.3x to 3.8x
for à-trous by keeping every level undilated and using on-chip memory.

**I1, the à-trous filter.** One pixel makes 75 image loads per level and 375 over five
levels. An 8x8 group at a step of one needs a 12x12 halo, which is 144 distinct positions
against 4800 loads. At a step of two the halo is 16x16 and the factor is still above six.
At a step of four the halo is 24x24 and the factor falls to under three.

Cache the three things a tap needs, for the three finest levels:

```glsl
/// What one tap of the kernel is, kept on chip.
///
/// **The three finest levels, and the coarsest two read as they always did.** A 8x8 group
/// needs a halo of 12, 16 and 24 a side at the first three steps, and 40 at the fourth —
/// which is 4800 taps of on-chip memory for a saving that has already fallen under three.
/// `ATROUS_TILE` is the specialization constant the pass sets, and nought turns the tile
/// off.
struct AtrousTap
{
    vec3 mNormal;
    vec3 mPosition;
    vec3 mColour;
};

shared AtrousTap sTile[ATROUS_TILE * ATROUS_TILE];
```

A 24x24 tile holds 576 taps of nine floats, which is 21 kB against the 48 kB the device
gives. Two pipelines come out of one module through the constant, and `AtrousPass::record`
picks one per level.

This also removes the 25 `rayAt` calls a pixel makes per level, because `mPosition` is
filled once per tile entry rather than once per tap.

**What it does not reach.** The two coarsest levels are untiled and are then half of what
is left. Dolp and others answer that with a permutation before and after each level, which
makes every level undilated and puts all five inside one small tile, for 1.3x to 3.8x over
the whole filter. That changes which invocation owns which pixel, so it is the step after
this one and not part of it.

**I2, the fog integrator.** One column thread reads a 3x3 tent from two volumes at each of
64 slices. An 8x8 group fetches 576 values per image per slice for 100 distinct ones. Hold
a 10x10 tile of `fogScatter` and `fogSunward` per slice, which is 3.2 kB, and refill it as
the loop rolls `next` forward. `before` and `here` stay in registers, so only one tile is
live.

**Gate.** Bit-identical picture for I2, whose arithmetic does not change. For I1 the
arithmetic does not change either, so the same gate applies. If either moves the picture,
the tile is indexed wrongly at its edge. Time each alone, with a thrown-away warm-up leg
first, and record both numbers.

### Fix J. Narrow the formats the evidence allows

**Files.** `rtx/shaders/gbuffer.h`, `composite.comp`, `tone.comp`, `histogram.comp`,
`atrous.comp`, `accumulate.comp`, `image.cpp`, `tracechain.cpp`, `vulkanrenderer.cpp`.

**The source.** Streamline's Ray Reconstruction guide gives the accepted format of each
input. The colour takes "any standard 3-channel format". Motion vectors take "RG16_FLOAT
or RG32_FLOAT". Normals take "RGB_F16 or RGB_F32".

**J1. `GBUFFER_RADIANCE` from `rgba32f` to `rgba16f`.** `Direct` and `Indirect` write 32
bytes a pixel between them. `MAX_SUN_RADIANCE` is 1000 and FP16 reaches 65504, and the
exposure is applied after the composite, so nothing in these two images approaches the
limit. `ACCUMULATE_COLOUR` is already `rgba16f`, so the indirect channel is narrowed one
pass later in any case. This saves 16 bytes a pixel.

**J2. `GBUFFER_MOTION` from `rg32f` to `rg16f`.** Three images carry motion. FP16 holds a
relative precision of one part in 2048, so a motion of 100 pixels lands within 0.05 of a
pixel and a full-screen sweep within one. NVIDIA lists the format. This saves 12 bytes a
pixel.

**J3. The upscaled target from `R32G32B32A32_SFLOAT` to `R16G16B16A16_SFLOAT`.** At
3840x2160 that image is 133 MB, DLSS writes all of it and the tone pass reads all of it.

**What must stay FP32, and why.** The composite at `tracechain.cpp:57` is what
`FrameImage::Composite` copies out, and that is the image a measured difference is read
from. Narrowing it would narrow the instrument. Leave it, and say so in the comment. The
offline `sum` image accumulates hundreds of frames and genuinely needs the range.

**Watch this.** `image.cpp:48` holds a format switch for `readImage`. `GBUFFER_DEPTH` stays
`rg32f`, because the second channel is a world distance and `frame.mFar` runs to six
figures.

**Gate.** The picture moves in the last bits. Read it with `--exposure=1` and as floats.
Ten pairs. Take J1, J2 and J3 as three separate readings, because one of them may be the
one that matters.

### Fix K. Two defects and the header unification

**K1.** `wavepass.cpp` declares four bindings in `sComposeBindings`. `wavecompose.comp`
declares three and the pass pushes three. Delete the fourth.

**K2.** `RTX_SHADER_LIB` in `components/rtxvulkan/CMakeLists.txt` omits `froxel.glsl`,
`medium.glsl`, `starfield.glsl` and `texturearray.glsl`. Add them. Add `basis.glsl` when
Fix D creates it.

**K3.** Sixteen headers repeat the host alias block. Put it in one header beside
`portable.h` and include it from there.

**K4.** Five headers define and undefine a `uint64` macro, and the order works by accident
of the include graph. Define it once in `portable.h` and never undefine it.

**K5.** Add one bounds helper and use it in all eleven compute shaders, which spell the
same test four ways.

**K6.** Split `StarField`, `SkyPatch`, `CloudDeck`, `MoonDisc`, `SkySource` and the weather
constants out of `visibility.h` into a `sky.h`. `tone.h` and `fogvolume.h` then stop
pulling the whole scene description, and `tone.comp` stops declaring a 64-bit extension it
has no use for.

**K7.** Use `computeBinding` and `computeBindings<N>` in `wavepass.cpp`, which spells the
same structure by hand three times.

**Gate.** Nothing may change. `ninja` must build, the `Rtx*` filter must pass, and the
frame hash must not move.

### Fix L. The two cheap arithmetic wins

**L1.** `paintedOver(painted, crossings)` computes `1 - pow(1 - x, crossings)`. An oriented
sprite always passes a `crossings` of one, where the answer is `x`. Test for it. The test
is uniform over a whole emitter run, so it is coherent.

**L2.** `sprites.glsl:634` and `:635` take `pow(layer, a)` and `pow(layer, b)`. Take
`log2(layer)` once and `exp2` twice.

**Gate.** L1 is exact at a `crossings` of one. L2 is exact to the last bit only if the
compiler already fused them, so check the frame hash and accept a move of one part in 255.

### Fix M. Allocate the layer channels only where they are written

**Files.** `gbuffer.cpp`, `gbuffer.hpp`, `vulkanrenderer.cpp`.

**The change.** `Transparency`, `TransparencyOpacity` and `TransparencyMotion` are written
only where `frame.mLayerCompositedAfter` is set, which is where the frame is upscaled. Give
`GBuffer` a flag and point the three at a 1x1 stand-in otherwise. `CompositePass::mNoSum`
and `TonePass::mNoBloom` are the pattern, twice over.

**What it saves.** 20 bytes a pixel of device memory, which is 41 MB at 1920x1080, and six
of the 28 G-buffer barriers.

**Gate.** No picture change with DLSS on. With DLSS off, the three images are never read,
so nothing may change there either.

### Fix N. Measure a ballot against the shared word

**Files.** `spriteruns.comp`, `spritestarts.comp`, and the requirements the device asks for.

**The source.** GPUOpen's compaction note says a ballot beats a shared atomic inside one
wave. The Khronos subgroup tutorial says subgroup traffic costs about what ALU costs.
`SPRITE_RUNS_LANES` is 32, which is the subgroup size this device reports.

**What the tree already says.** `spriteruns.comp:22` calls a ballot "the same thing with a
device requirement attached". `bindings.glsl:137` records that a subgroup reduction for the
hit counter measured no better. **That measurement does not carry over.** It was one atomic
on 5.5 per cent of rays. This is a shared atomic per match and a barrier per 32 sprites
across every tile, and a 1024-lane scan with two barriers a step.

**The requirement.** `VK_KHR_shader_subgroup_ballot` is Vulkan 1.1 core. Every card at or
above the Turing floor this fork states has it. The floor is already a hard failure that
names what is missing, so one more named requirement changes nothing about the shape of
that failure.

**The change.** Write both, measure both, keep the winner, and record the number either
way. Balmora at night in the rain is the place.

### Fix O. Do not serialize the sprite shading on one lane

**Files.** `spriteshade.comp`.

**The change.** `sortRun` is a bitonic sort over device memory with a barrier per stage.
The main loop then runs one step per sprite, and in each step lane 0 alone reads the grid
while 1023 lanes wait. Sort in shared memory where the emitter's count fits the workgroup,
which is 8 kB for 1024 eight-byte keys. Batch the per-sprite read so 32 lanes serve 32
sprites at a time.

**Watch this.** The accumulation order into `grid[]` decides the answer. A batch must keep
the front-to-back order the sort produced. This is the change most likely to be wrong in a
way the frame hash catches only sometimes.

**Gate.** Bit-identical picture over ten pairs, and a measured number at Balmora at night
in the rain.

---

## 4. The plan

Ten steps. Each is one commit. Each names its verification chain and its gate. Do not
start a step before the step before it is green.

The chain every step below means by "the full chain" is the one this tree states:

```
ninja -C build-debug <the targets the step touched>
./build-debug/components-tests --gtest_filter='Rtx*'
CLANG_FORMAT=clang-format-14 CI/check_clang_format.sh
apps/rtxtool/repeatable.sh --pairs=2      # while you work
apps/rtxtool/repeatable.sh --pairs=10     # before the step is done
```

`--pairs=10` is not optional on a step whose gate is a picture. A pair that finds nothing
has found nothing.

### Step 1 — the two defects

Fix K1 and K2. No behaviour, no measurement.

**Verify.** `ninja -C build-debug`. `CI/check_clang_format.sh`.
**Gate.** The build is green and nothing moved.

### Step 2 — the header unification

Fix K3, K4, K5, K6, K7.

**Verify.** `ninja -C build-debug`, then
`./build-debug/components-tests --gtest_filter='Rtx*'`, then format.
**Gate.** Every `static_assert` in the shared headers still holds. The frame hash from
`apps/rtxtool/repeatable.sh --pairs=2` matches the hash before the step.

### Step 3 — the cheap wins with no picture in them

Fix H, then Fix L, then Fix G.

Take H first, because it touches only host code and the validation layers are the gate.
Take G last in this step, because it moves two `static_assert` figures and those are worth
their own commit hunk.

**Verify.** `ninja -C build-debug`, `components-tests --gtest_filter='Rtx*'`,
`./build-debug-asan/openmw-rtxtool shot --view=seyda-neen-ship` under the ASAN options the
script sets, then format.
**Gate.** `repeatable.sh --pairs=2` while you work and `--pairs=10` at the end. The scene
columns repeat exactly. The picture does not move.

### Step 4 — resolve the triangle once

Fix E.

**Verify.** `ninja -C build-debug`, `components-tests --gtest_filter='Rtx*'`, then format.
**Measure.** One thrown-away `bench` leg, then three interleaved pairs at
`seyda-neen-ship` and three at the Balmora mages' guild.
**Gate.** Bit-identical picture. If the trace time rises, the extra two words of `Hit` beat
the saved loads. Say so and revert the step rather than keep it.

### Step 5 — the shadow ray's cone

Fix A1 only. Leave A2 for later.

**Verify.** Full chain. `openmw-rtxtool check`.
**Measure.** Three interleaved pairs at the Balmora mages' guild, which has the lamps, and
three at an exterior with trees.
**Gate.** The scene columns repeat exactly. The picture moves and should move: read a still
with `--exposure=1` and confirm the change is a softened cutout in shadow and nothing else.
Ten pairs.

### Step 6 — the sky source and the cone level

Fix B, then Fix C. B first, because C depends on nothing and B gives A its footprint
parameter a place to live.

Write the cone test before Fix C: a `Rtx*` case that asks `coneLod` for a level with a
width of nought and asserts the sampled texel is the level-zero texel. That is the one
behaviour the branch removal has to preserve.

**Verify.** Full chain, plus the new test.
**Gate.** Bit-identical picture for both. If B moves the picture, the rewrite reassociated
an addition. Find it before you go on.

### Step 7 — the two tiles

Fix I2 first, then Fix I1. I2 is the smaller change and its tile is a fixed 10x10.

Fold Fix F18 from the review into I1: read the à-trous inputs through a sampled image with
`texelFetch` rather than through `imageLoad`, in the same commit, and measure the pair once.

**Verify.** Full chain.
**Measure.** Each alone, with a thrown-away warm-up leg first. `profile.sh` for the CPU and
`nsys profile` for the GPU timeline if a number surprises you.
**Gate.** Bit-identical picture for both, because neither changes the arithmetic. Record
the before and after for each pass.

### Step 8 — the format narrowing

Fix J1, then J2, then J3. Three commits, three readings.

**Verify.** Full chain after each.
**Gate.** Take each reading with `--exposure=1` and read the floats. Ten pairs each. State
the bytes saved per pixel and the measured frame time beside each other.

### Step 9 — the fog column and the stand-in channels

Fix F, then Fix M.

**Verify.** Full chain.
**Measure.** Balmora at night in the rain for F, which is where the sprite loop is loaded.
**Gate.** Bit-identical picture for F. For M, nothing may change with DLSS on and nothing
may change with it off.

### Step 10 — the three that change what the frame draws or what the device must have

Fix D, then Fix N, then Fix O. One commit each, one reading each.

Take Fix D last of the three that must land, because it moves every drawn direction and a
reading taken with it in flight tells you nothing about anything else.

**Verify.** Full chain.
**Gate.** For D: the estimator stays unbiased, so a settled frame must agree. Compare a
64-frame accumulation before and after and state the difference. For N and O: bit-identical
picture, and a recorded number whichever way the measurement falls.

### Not in the plan

- **F11, the payload word in place of the hit records.** Withdrawn. NVIDIA's guidance says
  keep the payload small and says hit records are the efficient carrier.
- **F30, the two masks in one image.** Withdrawn. NGX takes two parameters.
- **Narrowing the composite image.** It is the instrument a measured difference is read
  from. Leave it at FP32 and say so where it is made.
- **Narrowing `GBUFFER_DEPTH`.** Its second channel is a world distance and `frame.mFar`
  runs to six figures.
- **Fix A2, the source's angular spread on a shadow ray.** It is right and it is a picture
  change on top of a picture change. Take it after Step 5 has a number.

---

## 5. Risk register

| Step | What could go wrong | How it shows | What to do |
|------|---------------------|--------------|------------|
| 3, Fix G | The two `static_assert` figures are guessed and wrong | The build fails at the assert | Read the new figures from the failure, do not compute them by hand |
| 3, Fix H | A batched barrier drops a dependency | The asan build or the validation layers report it, or a frame flickers | Run the asan build. Never batch across two different source stages without checking |
| 4, Fix E | `Hit` grows and the trace loses occupancy | The trace time rises with no other change | Revert. Say the number |
| 5, Fix A1 | A footprint is passed that is not the asking ray's | A shadow softens where it should not | Read a still with `--exposure=1`. The wrong footprint blurs a near shadow, not a far one |
| 6, Fix C | A zero width reaches `log2` and the driver gives a NaN | A black or white texel where a cutout stood | The test written before the change is what catches this |
| 6, Fix B | An addition is reassociated | The picture moves where it must not | Compare the two expressions term by term. There are only three sources |
| 7, Fix I1 | The tile is indexed wrongly at its edge | The picture moves in an eight-pixel grid | The gate is a bit-identical picture, which catches it |
| 8, Fix J | FP16 clips a value the exposure later needs | A highlight flattens | `MAX_SUN_RADIANCE` is 1000 against a limit of 65504. Check the sun disc alone |
| 10, Fix D | Nothing, except that everything moves | Every stochastic term differs | Compare a settled 64-frame accumulation, not a single frame |
| 10, Fix O | A batch loses the front-to-back order | A puff lights from the wrong side, sometimes | Ten pairs. This one needs them |

---

## 6. What the plan is worth

The bytes a pixel writes, before and after, at the traced resolution:

| Channel | Now | After J1 and J2 |
|---------|-----|-----------------|
| Direct, Indirect | 32 | 16 |
| Motion, ReflectionMotion, TransparencyMotion | 24 | 12 |
| Albedo, Specular, Guide | 24 | 24 |
| Depth | 8 | 8 |
| ParticleMask, BiasMask, StarsShown | 6 | 6 |
| Transparency, TransparencyOpacity | 12 | 12 |
| **Total** | **106** | **78** |

That is 28 bytes a pixel, which is a quarter of the G-buffer's store traffic. At 1920x1080
and 60 frames a second it is about 3.5 GB each second.

The loads a pixel makes in the two filter passes, before and after Fix I:

| Pass | Now | After |
|------|-----|-------|
| à-trous, five levels | 375 | about 200 |
| fog integrate, per column | about 1170 | about 200 |

**The à-trous figure stops where the halo outgrows the tile.** A 8x8 group needs a 12x12
halo at a step of one, 16x16 at two and 24x24 at four. The three finest levels fit a 24x24
tile at 21 kB, and the two coarsest do not. Those two are then half of what is left, which
is why 375 falls to 200 and no further.

**The permutation is what takes the last half.** Dolp and others apply a permutation before
and after each level so that every level is undilated, which puts all five inside one small
tile. They report 1.3x to 3.8x over the whole filter. Take the plain tile first, measure it,
and treat the permutation as the step after — it changes which invocation owns which pixel,
which is a larger change than a tile.

Everything else on the list is arithmetic, barriers, headers and two defects. None of it
was measured here, and every step above says which measurement decides it.

---

## 7. What was implemented, and what the measurements said

Every change below is in the tree. Every one passed the same three gates: the 622 `Rtx*`
tests, `openmw-rtxtool verify --against` over 23 views, and
`apps/rtxtool/repeatable.sh --pairs=10`. `openmw-rtxtool check` reports 24 of 24.
`--sync-validation` is quiet on both the upscaled and the unupscaled path.

### Landed

| Fix | What it did | Picture | Time |
|-----|-------------|---------|------|
| K1 | Deleted the fourth `sComposeBindings` entry that no shader declares | identical | — |
| K2 | Added the four absent `lib/*.glsl` and the nine absent `shaders/*.h` to CMake | identical | — |
| K3 | `hosttypes.h`: one host alias block for sixteen headers | identical | — |
| K4 | The `uint64` macro defined once and never undefined | identical | — |
| K5 | `lib/pixels.glsl`: one `outsideOf` for twelve compute shaders | identical | — |
| K6 | `sky.h` split out of `visibility.h` | identical | — |
| K7 | `computeBinding` in `wavepass.cpp` | identical | — |
| H | `Barriers` in the fog volume, the bloom, the accumulator and the sea | identical | — |
| L | `paintedOver` skips the power at one crossing, one logarithm for two | 1 of 255 on 0.00% of one view | — |
| G | The wave texel and the froxel grid sent, four `textureSize` gone | identical | — |
| E | `Hit` carries the corners, `resolveFor` reads them | identical | neutral |
| B | `SkyChoice` replaces two `float[3]` locals and three `skySourceAt` calls | identical | neutral |
| F | `FogRay` hoists the per-ray exponential out of the sprite march | identical | neutral |
| F18 | The à-trous taps read through the texture unit | identical | **5–6% off the filter** |

**The one number that moved.** With `--upscale=off`, where the cascade runs, three
interleaved pairs:

| Place | Before | After |
|-------|--------|-------|
| balmora-mages-guild, filter ms | 2.14 / 2.18 / 2.20 | 2.05 / 2.06 / 2.06 |
| seyda-neen-shore, filter ms | 1.46 / 1.48 / 1.50 | 1.40 / 1.41 / 1.42 |

Every pair favours the sampled read, and the after leg holds its number where the before
leg drifts up with the card. The trace is 0.03 ms higher in the same runs, which is inside
the drift and is the only place Fix E could show.

**Proof that steps 1 and 2 changed no code.** Both builds were disassembled with
`spirv-dis`, the debug instructions were dropped, and the opcode stream of all 31 modules
compared. Not one differs. The twelve bounds tests of K5 are the one intended instruction
change, and `verify` says they draw the same picture.

**The six `float[3]` spills are gone.** `visibilitysurface.rchit.spv` held six
function-scope `float[3]` variables before Fix B and holds none after.

### Tried, measured, and reverted

**Fix A1, the shadow ray's cone. Reverted.** The finding was wrong and the tree's original
comment was right. Three interleaved pairs with the footprint passed down:

| Place | Level zero | Cone level |
|-------|-----------|------------|
| balmora-mages-guild | 1.42 / 1.46 / 1.49 | 1.45 / 1.47 / 1.49 |
| ald-ruhn | 0.86 / 0.86 / 0.87 | 0.86 / 0.89 / 0.89 |
| seyda-neen-shore | 1.16 / 1.18 / 1.17 | 1.17 / 1.33 / 1.29 |

JCGT 10(1) 2021 measures level zero slower than a cone level, and that finding is about
the *fetch*. This path's cost is the *level*: `coneLod` returns at once for a width of
nought, so level zero here skips a texture-header read, a determinant and three logarithms
at every candidate. What the early return saves is more than the cache gives back. The
reason is now written above `lightThrough`, with the figures.

### Not taken, and why

- **F11, the payload word in place of the hit records.** NVIDIA's guidance says keep the
  payload small and names hit records as the efficient carrier. Withdrawn before any code.
- **F30, the two masks in one image.** `dlsspass.cpp:172` sets `pInIsParticleMask` and
  `pInBiasCurrentColorMask`, which are two NGX parameters.
- **J1, `GBUFFER_RADIANCE` to `rgba16f`.** `atrous.h` records the argument that holds it at
  full width: a reference is accumulated through both radiance channels, and that is a term
  added to a thousand others. Narrowing the instrument to save bandwidth is the wrong way
  round in a tree whose stated order puts precision above performance.
- **J2, motion to `rg16f`.** NVIDIA accepts the format, but `previousScreen` divides by a
  depth it only tests for sign, so a point just in front of the previous camera plane
  produces an unbounded motion — finite in FP32 and infinite in FP16. The clamp that would
  make this safe is its own change with its own justification.
- **J3, the upscaled target to FP16.** It needs the tone pass to read an unformatted image,
  because the same binding takes the FP32 composite when the upscaler is off. Deferred with
  J2.
- **Fix C, the cone-level split.** Its branch removal is now known to be wrong: the
  `coneWidth <= 0.0` early return is what makes a shadow ray cheap, which is what the A1
  measurement proved. The texture-dependent split alone is still available and untried.
- **Fix I1's shared-memory tile, Fix I2, Fix D, Fix M, Fix N, Fix O.** Untried.

### What the measurements changed about the plan

**The GPU zones say where the frame is.** At `--upscale=quality` over the Balmora mages'
guild: upscale 2.61, trace 1.46, tlas 0.19, bloom 0.13, air 0.12, refit 0.10, column 0.06,
tone 0.05, composite 0.03, exposure 0.03, sprites 0.02, skin 0.02. With `--upscale=off`:
trace 3.28, filter 2.19, accumulate 0.38, air 0.21.

Three things follow.

1. **The à-trous cascade does not run on the shipping path.** Ray Reconstruction replaces
   it. Fix I1's remaining half is worth 2 ms on a diagnostic path and nothing on the one
   the budget is written against.
2. **Fix I2 targets a 0.06 ms pass.** Six times fewer fetches of 0.06 ms is 0.05 ms. It is
   no longer worth the shared-memory tile it would take.
3. **Every ALU change measured neutral.** Fix E, Fix B and Fix F each remove real work —
   an index fetch per hit, six local-array spills, an exponential per sprite — and not one
   moved the trace. The compiler was already doing it. What did move was the one change
   about *where a read goes*, which is the one thing the compiler cannot choose.

**The rule this session produced.** On this hardware and this tree, arithmetic in a shader
is free and the path a read takes is not. Rank a finding by which of the two it is before
ranking it by how much work it removes.
