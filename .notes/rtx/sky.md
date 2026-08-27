# The sky, and what each layer of it owes

Two faults, seen from several sides. **The sky is composited in the wrong order**, so a layer hides
what it should not and shows through what it should. And **every layer is drawn by one rule and
lights by another**, the two written at different times.

| layer | drawn by | hidden behind a moon | hidden behind the deck | lights by |
| --- | --- | --- | --- | --- |
| the dome | `skyGradient` | no, and rightly — it is the air in front | yes | `skyGlow` |
| the fill | nothing | — | — | `skyGlow`, via `mSkyFill` |
| star field, nebulae, constellations | `starField`, `skyPatches` | **no, and wrongly** | yes | **nothing** |
| the sun's disc | `skyRadiance` | yes, by `hidden` | yes | `gather` |
| the moons | `moonFace` | first over second: **no** | yes | `gather` |
| the cloud deck | `cloudDeck`, as an emission | — | — | **nothing**, and **nothing lights it** |

The engine's own order is the one to match, and it is `SkyManager::create`'s: atmosphere, night sky,
sun, Masser, Secunda, clouds. Everything drawn later takes its share of everything drawn earlier,
which `paintMoon` does by writing `color.a = maskAlpha` under a `(ONE, ONE_MINUS_SRC_ALPHA)` blend.

**And what the sky delivers should be stated once, with every layer drawn out of it.** `skyFill` is
the first half of that and knows about none of the others: it makes the dome deliver what
`Ambient_<weather>_Night_Color` says a night is worth, and anything else that starts lighting has to
come out of the same figure or the night gets brighter again.

Measured at a clear midnight, linear luminance: the weather's ambient is **0.0168**, the dome is
**0.0030**, so the fill is **0.0138**. On day 0 the moons add **0.0125** perpendicular, **0.0113**
onto flat ground.

---

## 1. Stars shine through the moons

**Root cause, and it is a compositing order.** `skyRadiance` adds the star field and the patches,
then adds each moon on top, then dims the sun by `hidden` — the largest coverage any moon reached.
So a moon takes its share of the sun alone. The comment above the loop says as much: *what the moon
does hide is anything further off than it is, which for now is the sun alone.*

The rasterizer does not have this. It draws the night sky before the moons and `paintMoon` writes
`color.a = maskAlpha` under `(ONE, ONE_MINUS_SRC_ALPHA)`, so an opaque moon replaces whatever the
star sheet put behind it. Rendered at twelve degrees of field, ours has stars sitting on Masser's
face.

**The fix, and it deletes more than it adds.** Split what a ray that reached nothing finds into the
air in front of the moons and everything behind them:

- **behind**: the star field, the patches, and the sun's disc — all on or beyond the celestial
  sphere.
- **in front**: the dome's own gradient, which is the air the moon is seen through and is why the
  moons are added rather than composited today.

Then each moon in slot order composites over *behind* by its own `covered`, which is Masser first and
Secunda second — the order the engine creates them in. `hidden` goes away with it, and two moons that
overlap come out right for the first time rather than taking the deeper one's maximum.

- Test: a ray down the middle of an opaque moon returns the moon and no star, whatever the sheet
  says; and one a hair outside the limb returns the star.

---

## 2. The moons still do not rise

**They arrive thirty degrees up, and now they arrive gently.** `MoonMoment::mShadowBlend` fixed how a
moon appears and not where. Both halves of the engine's horizon treatment are still in force:
`earlyShadowAlpha` draws nothing at all under `Fade_End_Angle` — 30 degrees for Secunda, 40 for
Masser — and `mShadowBlend` shows no face until the same angle. The engine does exactly this, so it
is faithful, and it is also the one piece of its sky that is plainly a workaround for a bright quad
over a fogged dome. A moon should come up out of the horizon.

**What replaces it has to be the air**, because there is nothing else between an eye and a moon. The
content still decides everything about the moon: where it rises, how fast it climbs, its phase, and
the hour it fades out in daylight. What is added is that the air is modelled instead of the moon
being switched off.

- `MoonMoment` gains the hour's own fade as a field of its own. `mAlpha` stays exactly as it is,
  because the rasterizer reads it and its behaviour is never changed.
- The ray tracer takes that field and gates on `mAlongArc > 0`, which is already the engine's way of
  saying a moon is on its arc rather than waiting to rise or already set.
- `mShadowBlend` stops reaching the ray tracer, for the same reason `earlyShadowAlpha` does: the two
  are one mechanism and keeping half of it would hold the face out until thirty degrees anyway.
- Rayleigh optical depth at the three sRGB primaries — **0.068, 0.097 and 0.221** at the zenith,
  which is `0.008569 λ^-4` with its usual correction — times the Kasten-Young air mass
  `1 / (sin h + 0.50572 (h + 6.07995)^-1.6364)`, scales the disc and the light alike.

What that gives, as transmittance per channel:

| elevation | air mass | R | G | B |
| --- | --- | --- | --- | --- |
| 90° | 1.00 | 0.934 | 0.908 | 0.802 |
| 30° | 1.99 | 0.873 | 0.824 | 0.644 |
| 10° | 5.59 | 0.684 | 0.582 | 0.291 |
| 5° | 10.31 | 0.496 | 0.368 | 0.102 |
| 1° | 26.30 | 0.167 | 0.078 | 0.003 |
| 0° | 37.92 | 0.076 | 0.025 | 0.000 |

So a moon comes up as a deep red ember, is orange at five degrees, and is itself by thirty. A full
Masser overhead loses 9% of its light, which is right and is the price.

**Not the sun, and say why.** The same air is over it, but `Sun_Disc_Sunset_Color` already reddens
the disc and `sunShareAt` already ramps it out, so extinction there would be the content's own
sunset counted twice. That is a question for whoever lights the cloud deck, which needs the sun's
air mass for its own reasons.

- Test: the disc and the light fall together, monotonically, from the zenith to the horizon; a moon
  off its arc gives nothing; and the published 407,000th still holds for a real moon overhead.

---

## 3. The night sky's sheets light nothing

**Root cause.** `skyGlow` returns the dome's gradient and the fill. `starField` and `skyPatches` are
called only from `skyRadiance`, so a bounce that escapes never finds them.

**What they are worth**, measured off the shipped files as the mean of `rgb * a` in linear light:

| sheet | mean | scale | over |
| --- | --- | --- | --- |
| `tx_stars` | 0.00137 | `STAR_RADIANCE` 0.18 | the whole dome |
| `tx_stars_nebula` | 0.00199 | `NEBULA_RADIANCE` 0.06 | its own patch |
| `tx_stars_nebula2` | 0.00098 | " | " |
| `tx_stars_nebula3` | 0.00127 | " | " |
| `tx_stars_warrior` | 0.00188 | " | " |
| `tx_stars_mage` | 0.00161 | " | " |
| `tx_stars_thief` | 0.00157 | " | " |

The field comes to 2.5e-4 and the six patches to about 1.3e-4, against the dome's 0.0030 — **13%**.

**Not by sampling them per ray.** A star field is 0.22% bright texels carrying nearly all of its
light, so a gather ray that lands on one returns 450 times the mean. That is a firefly in the
indirect light, and the sheets carry no mip chain to blur it away.

**The fix.** The mean, worked out at load and spent out of the same budget.

- `NightSky` gains the mean radiance each sheet is worth. Decode with `texelAt` and `AlphaImage`
  over a `TextureData`, which already read every format the game ships.
- A patch covers `1 - cos(t)` of the hemisphere, so its share of the sky's mean is that times its own
  mean. The field covers the whole dome.
- `skyFill` subtracts the sheets as well as the gradient, so the total the sky delivers is still the
  weather's ambient. The sheets change **where** the night's light comes from, not how much there is.
- Test: the sum of what the layers deliver equals the weather's ambient, whatever the sheets say.

---

## 4. The cloud deck is an emission and is never lit

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

1. Per-sheet mean luminance at load, beside the night sheets' means from step 3 — one reader serves
   both.
2. Coverage from the alpha, and from the texel's luminance against the mean where the alpha is flat.
3. The deck lit by the sky and by the moons, at a transmission of 0.25. Night first, because it is
   the case with the fewest terms and the one already known to read wrong.
4. The sun on the deck, with its own horizon and its own air mass. Day and dusk.
5. The deck's shadow on the world.
6. Delete `SkyContent::mLift` and `Sky::dayFog`, which exist only to light a deck that is no longer
   painted.

---

## 5. What terminates a path is what the open sky delivers

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

Steps 1 and 2 first and in that order. Both are regressions against the old renderer rather than
niceties, both are small, and both are in the frame every night.

Step 3 next, because step 4 needs the same image reader and the same budget rule, and it is far
cheaper to get both right on the sheets than on the deck.

Step 4 last, and in its own six steps. It is the one that can regress a day that has just been tuned.

Step 5 stays open.

## What must still hold at each step

- `openmw-rtxtool shot --hour 12` is unchanged in every step but 4, and in 4 it changes only where
  cloud is drawn.
- The sky's total delivered light equals the weather's ambient at night, whatever the layers say.
- `components-tests` and `openmw-tests` pass, and the formatting check is clean.
- No new allocation on the frame path. Everything read off a mesh or an image is read at load.
