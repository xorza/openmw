# The sky's light budget

Five open issues in `.notes/ISSUES.md` are one design fault seen from five sides. Every layer of the
sky is drawn by one rule and lights by another, and the two rules were written at different times.

| layer | drawn by | lights by |
| --- | --- | --- |
| the dome | `skyGradient` | `skyGlow` |
| the fill | nothing | `skyGlow`, via `mSkyFill` |
| star field, nebulae, constellations | `starField`, `skyPatches` | **nothing** |
| the cloud deck | `cloudDeck`, as an emission | **nothing**, and **nothing lights it** |
| the moons | `moonFace` | `gather`, and only above 30 or 40 degrees |

**What the sky delivers should be stated once, and every layer drawn out of it.** `skyFill` is the
first half of that and knows about none of the others: it makes the dome deliver what
`Ambient_<weather>_Night_Color` says a night is worth, and anything else that starts lighting has to
come out of the same figure or the night gets brighter again. That is the constraint every step
below is written against.

Measured at a clear midnight, linear luminance: the weather's ambient is **0.0168**, the dome is
**0.0030**, so the fill is **0.0138**. On day 0 the moons add **0.0125** perpendicular, **0.0113**
onto flat ground.

---

## 1. A moon appears thirty degrees above the horizon

**Root cause, and it is exact.** `MoonModel::earlyShadowAlpha` returns nought below
`Fade_End_Angle - Moon_Shadow_Early_Fade_Angle` and ramps to one over the half degree above it.
`placeMoon` maps `mAlongArc` straight to elevation, so a moon is not drawn at all until it stands
`Fade_End_Angle` above the horizon: **30 degrees for Secunda and 40 for Masser**.

At Secunda's 9 degrees an hour that ramp is 3.3 minutes wide, and on day 0 it rises at 19:12 and
arrives at **22:28**. Two shots either side, at 22:24 and 22:36, show an empty sky and then a whole
moon. Masser rises at 16:00 and arrives at 21:03.

**And the softening the engine does have was dropped.** `MoonMoment::mShadowBlend` crossfades a moon
from a sky-coloured disc to its painted face between `Fade_End_Angle` and `Fade_Start_Angle`.
`Rtx::makeMoon` reads four of the five fields and not that one, so the ray tracer has the hard gate
without the crossfade behind it.

**Why the engine does it.** A rasterized moon is a textured quad over a fog-coloured dome, and near
the horizon the two disagree. It is a workaround for a picture this renderer does not draw.

**The fix.** A moon is drawn whenever it is above the horizon, and the air it is seen through is what
dims it.

- Drop `earlyShadowAlpha` from what reaches `MoonPlacement::mAlpha`. Keep `hourlyAlpha`, which is the
  daylight fade and is about the hour rather than about the horizon.
- Let the fog extinction already on the frame attenuate a low moon's disc and its light, on the
  slant path through the air. `Fog::mExtinction` and the moon's own elevation are both to hand.
- `MoonModel` keeps `earlyShadowAlpha` and `shadowBlend` for the rasterizer, which still wants both.
- Test: a moon at one degree of elevation is drawn and lights, and one below the horizon does
  neither. And the arc between them holds no step — sample it every tenth of a degree and assert no
  neighbouring pair differs by more than the extinction can account for.

---

## 2. The night sky's sheets light nothing

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

## 3. The deck's horizon fade is a number this renderer chose

**Root cause.** `CLOUD_HORIZON = 0.28` is a smoothstep in `sin(elevation)` picked to hide the
stretch. The engine's own fade is `ModVertexAlphaVisitor::Clouds`, which writes vertex alpha by
index: 49 to 64 get nought, 33 to 48 get 0.251, the rest get one.

On the vanilla mesh, with its `NiTriShape` fifteen units under its `NiNode`, those are rings at
**4.86 and 15.06 degrees** of elevation, with full cover from the ring above at **27.39**. Ours
reaches 1.0 by 16.2 degrees and still draws 0.29 of a deck at 5.7, so it hangs cloud lower than the
engine does and reaches full cover higher.

**The fix.** `CloudShell` already walks that mesh and fits its shape. It can read the rings too.

- Collect the distinct ring elevations. The lowest is the deck's rim and the next is where the
  engine's quarter alpha sits; the one above that is full cover.
- `CloudDeck` carries the three, and the shader interpolates the engine's own 0, 0.251 and 1 between
  them instead of a smoothstep.
- A mesh with fewer than three rings keeps a rim and a full-cover elevation and no middle.
- Test: the synthetic layer in `cloudshell.cpp` gains rings at known elevations, and the fade is
  asserted at each of them and half way between.

---

## 4. The cloud deck is an emission and is never lit

**Root cause.** `CloudDeck::mColour` is the weather's air times what its own daylight says a cloud is
worth, and `cloudDeck` returns that times coverage. No sun, no moon, no sky reaches it, it casts
nothing, and it loses the sun at the same instant the ground does.

This is the largest of the five and the reference implementation has already been through it —
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

1. Per-sheet mean luminance at load, beside the night sheets' means from step 2 — one reader serves
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

Lowest priority of the five, and the only one whose fix is a real cost: it wants an occlusion
estimate the renderer does not keep. Note it, leave it, and revisit once the deck is lit — a deck
that shadows the world is the same machinery seen from another side.

---

## Order

Step 1 first: it is small, it is exactly reproducible, and it is the one a player sees every night.

Step 2 next, because step 4 needs the same image reader and the same budget rule, and it is far
cheaper to get both right on the sheets than on the deck.

Step 3 next: it is contained, it is content-derived, and it changes the picture where the deck meets
the haze rather than everywhere.

Step 4 last, and in its own six steps. It is the one that can regress a day that has just been tuned.

Step 5 stays open.

## What must still hold at each step

- `openmw-rtxtool shot --hour 12` is unchanged in every step but 4, and in 4 it changes only where
  cloud is drawn.
- The sky's total delivered light equals the weather's ambient at night, whatever the layers say.
- `components-tests` and `openmw-tests` pass, and the formatting check is clean.
- No new allocation on the frame path. Everything read off a mesh or an image is read at load.
