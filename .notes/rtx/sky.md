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

## 1. Stars are dim and soft, and both are one cause

**Morrowind's stars are crisp because they clip.** `paintAtmosphereNight` hands the sheet's own texel
to a `(SRC_ALPHA, ONE_MINUS_SRC_ALPHA)` blend, so a texel of white at full alpha lands on the frame
buffer at **1.0** — the display ceiling — over a night sky of 9, 10, 11 out of 255. Its sampler is
bilinear like ours and spreads that texel over three pixels by three just as ours does, but every one
of those pixels saturates, so what shows is a hard white dot. Clipping is doing the work.

**Nothing in this renderer clips.** The frame is high dynamic range and tone mapped, so the same
spread is visible as a spread — and the level never reaches the top of the curve either.

**Measured**, at the zenith on a clear midnight, 600 by 400 at a 25 degree field so a pixel is 0.0625
degrees against the sheet's tenth of a degree per texel:

| | brightest pixel | pixels over 0.25 |
| --- | --- | --- |
| `--upscale off` | 0.584 | 208 |
| `--upscale quality` | 0.449 | 74 |

So the sampling loses about half of a star before Ray Reconstruction sees it, and Ray Reconstruction
takes a further third of the peak and two thirds of the bright pixels. Against a content file that
puts a star at 1.0.

**The fix is that a star is a point source and should be drawn at the pixel's size, not the texel's.**
The sheet is magnified — a texel is a tenth of a degree and a pixel at 1920 by 1080 over a sixty
degree field is 0.0556 — so a bilinear tent spans two texels, which is three and a half pixels. Draw
it instead as: the nearest texel's own flux, spread over a footprint the ray cone sets. Then

- a star is one pixel wide however far the sheet is magnified, which is what crisp means;
- its peak rises as the spread narrows, at constant flux, which is where brighter comes from;
- **the sky's mean is unchanged**, so `NightSky::mGlow` and `skyFill` are untouched and the night's
  light budget still holds.

**Under minification the mean is already the right answer.** A pixel wider than a texel is a pixel
several stars fall into, which is the glow `mGlow` already states — so the two blend on the ratio of
the pixel to the texel, and the sheets' own mip chains are not needed for either end.

**Then the level can be anchored rather than pinned.** `STAR_RADIANCE` is 0.18 today because *a star
is never brighter than a full moon's disc*, which is a true bound and not a derivation. What the
content states is a star at the top of the display range on a near-black sky, so the anchor is that a
peak texel lands at the top of the tone curve under the night's own exposure. The old bound stays as
a bound.

**Ray Reconstruction is a third of it and is its own question.** A sub-pixel, high contrast, moving
point is the worst case for a temporal upscaler, and the sky reports no albedo to demodulate by. The
options are to give the sky's point sources something RR can hold on to, or to draw them after the
upscaler at output resolution — which is an architectural move and wants stating as one before it is
attempted. Do the sampling first and measure again: a star that is one pixel and three times brighter
is a different input to RR than the one measured above.

- Test: a peak texel reaches the same radiance whatever the field of view, which is what "the pixel's
  size and not the texel's" means and what a bilinear fetch cannot do. And the sheet's mean over the
  sky is unchanged by the whole change, which is what keeps the budget.

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
