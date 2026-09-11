# The à-trous cascade: what it is bound by, and the one change that moves it

Two open items were investigated: the function-scope arrays, and the shared-memory tile with
Dolp's permutation schedule. The first is done and measured neutral. The second is **withdrawn**:
its premise is false on this hardware, and the measurements that say so also say what to do
instead.

Every figure here is from `bench --views=balmora-mages-guild --upscale=off` on this box, three
interleaved repeats unless stated.

## What the cascade costs, level by level

| levels run | filter ms | marginal |
|---|---|---|
| 1 | 0.39 | 0.39 — step 1 |
| 2 | 0.81 | 0.42 — step 2 |
| 3 | 1.16 | 0.35 — step 4 |
| 4 | 1.66 | 0.51 — step 8 |
| 5 | 2.09 | 0.43 — step 16 |

**The cost is flat in the stride.** At step 1 an 8×8 group's 1600 taps cover 144 distinct texels,
eleven-fold reuse and every tap an L1 hit. At step 16 the taps are 32 pixels apart, nothing is
reused and nearly every tap is its own cache line. They cost the same to within ten per cent.

## Why that withdraws the tile and the permutation

The tile's whole argument is that the finest levels re-read the same texels and shared memory
removes the repeat. The table above says the repeat is already free — so there is no DRAM traffic
for a tile to recover, and the upper bound on what one can give is whatever the texture path adds
over a shared-memory read. That was measured directly:

| taps of | bytes a tap | fetches a tap | filter ms |
|---|---|---|---|
| source, guide, depth | 24 | 3 | 2.06 |
| source, guide-with-depth-in-it | 16 | 2 | **1.78** |
| source, one wide geometry fetch | 24 | 2 | 1.97 |

**Halving the fetch count at constant bytes gives nothing. Cutting the bytes gives 14 per cent.**
So the cascade is bound by bytes returned per tap, not by how many instructions return them and
not by where they come from. A tile moves those bytes from one half of the SM's unified data block
to the other. Dolp's permutation exists only to make every level tileable, so it goes with it.

## What to do instead — one geometry channel cut for the denoiser

Per tap the cascade reads three images: the light it is filtering (`ATROUS_CHANNEL`, 8 bytes), the
shading normal (`Channel::Guide`, 8 bytes, of which it reads `xyz`) and the distance
(`Channel::Depth`, 8 bytes, of which it reads `g`). Sixteen bytes of geometry are fetched to use
twelve, and they are fetched twice.

**`Channel::Guide` and `Channel::Depth` have exactly three readers**: `DlssPass`, and
`accumulate.comp` with `atrous.comp`. The denoiser does not run when something upscales — there is
no `filter` zone in an upscaled frame — so the two sets of readers never overlap, and the formats
are cut for the upscaler because NGX asks for them that way.

So give the denoiser its own:

```
Channel::Surface, VK_FORMAT_R32G32_SFLOAT
    .r   the shading normal, octahedral, two 16-bit unorms
    .g   the distance from the eye, in world units
```

Eight bytes for what the cascade actually compares, in one fetch. Per tap 24 bytes become 16,
which is the measured **0.30 ms of 2.05, fifteen per cent**.

**Gated the way the layer channels now are.** `GBuffer::carries` already answers "does this frame
carry that channel"; the flag here is the negation of the one the layer channels take, because the
denoiser runs exactly where Ray Reconstruction does not. An upscaled frame gets a one-texel
stand-in and pays nothing.

**What it costs.** Eight bytes a pixel and one store in the trace, in the frame that is not
upscaled — 16 MiB at 1080p, in the mode a reference is built in, where memory is not the
constraint. Nothing at all in the shipping frame.

**What must be checked.** The octahedral normal is a different rounding from the half floats it
replaces — finer, at about 5e-5 radians against 5e-4, but different — and `pow(dot, 128)` amplifies
the difference by 128. Expect the picture to move by a handful of pixels at one part in 255, the
way the cone-level split did. Read it with `--upscale=off --exposure=1` over the 23 views and
record the count.

**`Channel::Depth` cannot be gated off to pay for it.** Four tests in
`apps/components_tests/rtx/visibility/frame.cpp` read it back from a renderer that does not
upscale.

## The floor, and why it is 16 bytes

The light is `rgba16f` and its alpha is written as one and never read, so two of its eight bytes
are waste — but there is no three-component 16-bit storage format, and the four-byte packed float
formats carry six mantissa bits against the cascade's own 3e-4 error floor. The normal cannot go
below 16 bits an axis either: ten bits is a tenth of a degree, and the normal test cuts at six
degrees through a 128th power. So 8 bytes of light and 8 of geometry is where this stops.

## The function-scope arrays, done

**Root cause, and it is one thing.** `glslc` cannot subscript a constant composite, so it copies
the whole array into function storage — once per subscript *expression*, before any loop is
unrolled. That is why `[[unroll]]` does not undo it: a probe took the sixteen to fourteen and no
further. Three tables read at a loop counter cost `fogintegrate.comp` nine such copies, `KERNEL`
cost `atrous.comp` two and `offsets` cost `accumulate.comp` one.

**The fix is to state the value rather than look it up**: the B3 spline row is two selects on
`abs(offset)`, the bilinear corner is `ivec2(i & 1, i >> 1)` and its share a product of two, and
the tent's offset is `ivec2(i % 3, i / 3) - 1` with its weight two more selects. Every one is
exact in floating point, so the picture does not move.

| | before | after |
|---|---|---|
| `fogintegrate.comp` | 12 | 3 |
| `atrous.comp` | 2 | 0 |
| `accumulate.comp` | 2 | 0 |

**Three stay and they should.** `surfaces[9]` is nine measured depths rather than a table two
selects can state, and GLSL copies an array parameter by value, so it and the two copies `sliceAt`
takes are irreducible short of nine named scalars.

**And it is neutral on the clock**, measured over three interleaved pairs at both upscale settings:
filter 2.04 → 2.05 ms, accumulate 0.36 → 0.36, air 0.21 → 0.21. Thirteen scratch arrays were worth
nothing in time. Taken for the shape, and the shape is what the next reader inherits.
