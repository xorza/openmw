# The sky, and what each layer of it owes

**Every layer is drawn by one rule and lights by another**, the two written at different times.

| layer | drawn by | lights by |
| --- | --- | --- |
| the dome | `skyGradient` | `skyGlow` |
| the fill | nothing | `skyGlow`, via `mSkyFill` |
| star field, nebulae, constellations | `starField`, `skyPatches` | `skyGlow`, via `StarField::mGlow` |
| a star, up close | a bilinear fetch at three pixels wide | — |
| the sun's disc | `skyRadiance` | `gather` |
| the moons | `moonFace` | `gather` |
| the cloud deck | `cloudDeck`, as an emission | **nothing**, and **nothing lights it** |

The compositing order is settled, and so are the moons: `skyRadiance` builds what lies beyond them,
composites each over it in `SkyManager::create`'s own order, and adds the dome in front as the air —
which is also what now takes a low moon out, in place of the engine's own angle gate. What is left is
the second column.

**What the sky delivers is stated once, and every layer that lights comes out of it.** `skyFill`
holds the sky to what `Ambient_<weather>_Night_Color` says a night is worth, and it now subtracts the
dome and the night's sheets alike — so the stars started lighting and the night stayed where it was.
The deck is the last layer outside that rule.

Measured at a clear midnight, linear luminance: the weather's ambient is **0.0168**, the dome is
**0.0030**, so the fill is **0.0138**. On day 0 the moons add **0.0125** perpendicular, **0.0113**
onto flat ground.

---

## 1. The sky's point sources go through a temporal upscaler

**Measured**, at 1920 by 1080 on a clear midnight, counting pixels of the frame:

| | brightest | over 0.5 | over 0.25 |
| --- | --- | --- | --- |
| `--upscale off` | 0.973 | 1099 | 4867 |
| `--upscale quality` | 0.914 | 347 | 3743 |

And the pixel-wide crossfade that took the native frame from 140 pixels over half brightness to 245
is worth nothing once the frame has been through Ray Reconstruction — 1020 against 1020. A
sub-pixel, high-contrast, moving point is what a temporal upscaler is built to remove, and a star is
all three.

**What was already tried and is in the tree.** The crossfade steepens to a pixel and the level is
anchored where the content puts a star; between them the shipped path went from 0.627 to 0.914.
Asking for the mip level a minified pixel wants was tried and reverted — a mip of a sparse point
field is those points averaged with the dark around them, and it took the pixels over a quarter from
1020 to 108.

**The fix is that they should not go through it.** The options, in the order they are worth trying:

- Give Ray Reconstruction something to hold on to. The sky reports `noResponse()`, so it has no
  albedo to demodulate by and no roughness to say the pixel is resolved rather than noisy. Cheap to
  try and it may be most of it.
- Draw the sky's point sources after the upscaler, at output resolution, as their own pass. It is the
  complete answer and it is an architectural move: the pass needs the camera, the star field's own
  parameters, and whatever says a pixel reached the sky rather than the ground.

Neither is a constant to tune, which is why this is a step of its own rather than more of the last
one.

- Test: whatever lands, the same frame drawn with and without the upscaler agrees on the brightest
  star to within a tenth of the display range. Today it is a fifth apart.

---

## 2. The cloud deck is an emission and is never lit

**Root cause.** `CloudDeck::mColour` is the weather's air times what its own daylight says a cloud is
worth, and `cloudDeck` returns that times coverage. No sun, no moon, no sky reaches it, it casts
nothing, and it loses the sun at the same instant the ground does.

This is the largest of them and the reference implementation has already been through it —
`/home/xxorza/Projects/rtxmw/docs/design.md` §8.56 and §8.57. Read both before starting.

**The shape of the answer**, from there:

- **The sheet supplies shape and the light supplies colour.** Each of the nine sky textures is a
  photograph of a 2002 sky, so compositing one over the dome puts the sky in twice. What is usable is
  the alpha the artist drew the clouds with — and, for every overcast weather, whose alpha is 255
  everywhere, the texel's own luminance against the sheet's mean.
- **A cloud is darker than the sky it covers.** Plane-parallel theory puts a deck's transmission at
  0.2 to 0.3. At 0.9 a night deck was 90% of the sky it hid; at 0.3 it is a dark shape blotting out
  stars, and thin cloud is not dragged down with it because how much sky a wisp replaces at all is
  its own alpha.
- **The layer keeps the sun after the ground has lost it**, and crosses less air on the way, which is
  why a sunset cloud is gold rather than black.
- **And it casts**, at a coarse mip, letting about a quarter through.

**The trap this fork has and that one did not.** `CloudShell::mCurvature` is `k · h` fitted to
Morrowind's cap, and it comes to 0.0575. Read as a world radius that is `h / R = 0.128`, where the
Earth's is 7.8e-5 — so `sqrt(2h/R)` off it gives a sunset dip of **27 degrees** rather than the
fraction of one a real layer has. **The shell is a shape fit and not a planet.** Whatever decides how
long the deck keeps the sun has to come from somewhere else.

**Steps.** Land them in this order, checking with `shot` after each:

1. Per-sheet mean luminance at load, with `Rtx::meanTexel`, which the night sheets already use.
2. Coverage from the alpha, and from the texel's luminance against the mean where the alpha is flat.
3. The deck lit by the sky and by the moons, at a transmission of 0.25. Night first, because it is
   the case with the fewest terms and the one already known to read wrong.
4. The sun on the deck, with its own horizon and its own air mass. Day and dusk.
5. The deck's shadow on the world.
6. Delete `SkyContent::mLift` and `Sky::dayFog`, which exist only to light a deck that is no longer
   painted.

---

## 3. What terminates a path is what the open sky delivers

`mAmbient` used to be six times the dome it stood for a bounce of, so a surface in a crevice was lit
more than the open ground beside it. `skyFill` closed that: the sky now delivers the weather's
ambient exactly, and `pathEnd` is that same figure.

**Which leaves the other half of it.** A bounce ray that hits something is now shaded as though that
something saw the whole sky. Nothing accounts for how enclosed it is, so the term that is meant to
stand for the bounces nobody traces cannot darken a hole.

Lowest priority of them, and the only one whose fix is a real cost: it wants an occlusion
estimate the renderer does not keep. Note it, leave it, and revisit once the deck is lit — a deck
that shadows the world is the same machinery seen from another side.

---

## Order

Step 1 first. It is small, it is in the frame every night, and it is the one thing left that the old
renderer plainly does better — by an accident of its own clipping, but better.

Step 2 is the last of the sky, and it goes in its own six steps. It is the one that can regress a day
that has just been tuned, and `Rtx::meanTexel` and the budget rule are both in place for it now.

Step 3 stays open.

**And one question this leaves for step 2.** `Rtx::airTransmittance` is the moons' now and not the
sun's, because `Sun_Disc_Sunset_Color` and `sunShareAt` already redden and ramp that disc between
them. A lit cloud deck needs the sun's air mass for its own reasons, and whoever writes it has to
settle whether the content's sunset and the air's are the same sunset stated twice.

## What must still hold at each step

- `openmw-rtxtool shot --hour 12` is unchanged in step 1, and in step 2 only where cloud is drawn.
- The sky's total delivered light equals the weather's ambient at night, whatever the layers say.
- `components-tests` and `openmw-tests` pass, and the formatting check is clean.
- No new allocation on the frame path. Everything read off a mesh or an image is read at load.
