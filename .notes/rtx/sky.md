# The sky's light budget

The open issues in `.notes/ISSUES.md` are one design fault seen from several sides. Every layer of the
sky is drawn by one rule and lights by another, and the two rules were written at different times.

| layer | drawn by | lights by |
| --- | --- | --- |
| the dome | `skyGradient` | `skyGlow` |
| the fill | nothing | `skyGlow`, via `mSkyFill` |
| star field, nebulae, constellations | `starField`, `skyPatches` | **nothing** |
| the cloud deck | `cloudDeck`, as an emission | **nothing**, and **nothing lights it** |
| the moons | `moonFace` | `gather` |

**What the sky delivers should be stated once, and every layer drawn out of it.** `skyFill` is the
first half of that and knows about none of the others: it makes the dome deliver what
`Ambient_<weather>_Night_Color` says a night is worth, and anything else that starts lighting has to
come out of the same figure or the night gets brighter again. That is the constraint every step
below is written against.

Measured at a clear midnight, linear luminance: the weather's ambient is **0.0168**, the dome is
**0.0030**, so the fill is **0.0138**. On day 0 the moons add **0.0125** perpendicular, **0.0113**
onto flat ground.

---

## 1. The night sky's sheets light nothing

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

1. Per-sheet mean luminance at load, beside the night sheets' means from step 1 — one reader serves
   both.
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

The moon that arrived whole thirty degrees up is done: it was `MoonMoment::mShadowBlend`, which the
engine has and the ray tracer was not carrying. Nothing was invented for it and nothing else moved.

The deck's horizon fade is done too. It was `ModVertexAlphaVisitor::Clouds` reduced to the three
crossing radii its bands stop at, read off the same mesh `CloudShell` already walks — 1.17, 1.72 and
2.50 tiles — with the engine's own `paintClouds` mixing the deck's colour toward the fog over the
same stretch. `CLOUD_HORIZON` is gone.

Step 1 first, because step 2 needs the same image reader and the same budget rule, and it is far
cheaper to get both right on the sheets than on the deck.

Step 2 last, and in its own six steps. It is the one that can regress a day that has just been tuned.

Step 3 stays open.

## What must still hold at each step

- `openmw-rtxtool shot --hour 12` is unchanged in every step but 2, and in 2 it changes only where
  cloud is drawn.
- The sky's total delivered light equals the weather's ambient at night, whatever the layers say.
- `components-tests` and `openmw-tests` pass, and the formatting check is clean.
- No new allocation on the frame path. Everything read off a mesh or an image is read at load.
