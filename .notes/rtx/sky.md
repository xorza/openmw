# The sky, and what each layer of it owes

**Every layer is drawn by one rule and lights by another**, the two written at different times.

| layer | drawn by | lights by |
| --- | --- | --- |
| the dome | `skyGradient` | `skyGlow` |
| the fill | nothing | `skyGlow`, via `mSkyFill` |
| the star field | the display pass, at output resolution | `skyGlow`, via `StarField::mGlow` |
| nebulae, constellations | `skyPatches` | `skyGlow`, via `StarField::mGlow` |
| the sun's disc | `skyRadiance` | `gather` |
| the moons | `moonFace` | `gather` |
| the cloud deck | `cloudDeck` | **nothing**, and it is lit by `deckLight` |

The compositing order is settled, the moons rise through the air rather than being switched off, and
the star field is drawn by the display pass at the resolution it is shown at — the three upscale
settings now put its brightest pixel at 0.980 apiece where they used to run from 0.973 down to 0.864.
What is left is the second column.

**What the sky delivers is stated once, and every layer that lights comes out of it.** `skyFill`
holds the sky to what `Ambient_<weather>_Night_Color` says a night is worth, and it now subtracts the
dome and the night's sheets alike — so the stars started lighting and the night stayed where it was.
The deck is the last layer outside that rule.

Measured at a clear midnight, linear luminance: the weather's ambient is **0.0168**, the dome is
**0.0030**, so the fill is **0.0138**. On day 0 the moons add **0.0125** perpendicular, **0.0113**
onto flat ground.

---

## 1. The cloud deck casts nothing, and loses the sun with the ground

**Where it stands.** The deck is lit rather than painted: `Rtx::deckLight` gives it the sky, the
moons and the sun, and `CLOUD_TRANSMISSION` is what a layer of droplets keeps of the three. What is
left is that it casts no shadow on the world, and that it is handed the ground's own sun — so it goes
out at the instant the ground does, where a real layer keeps the sun past the ground's horizon.

This is the largest of them and the reference implementation has already been through it —
`/home/xxorza/Projects/rtxmw/docs/design.md` §8.56 and §8.57. Read both before starting.

**The shape of the answer**, from there:

- **The sheet supplies shape and the light supplies colour**, and the shape half of that is in.
  Each sky texture is a photograph of a 2002 sky, so compositing one over the dome puts the sky in
  twice. What the deck takes instead is the alpha the artist drew the clouds with and the texel's own
  luminance against the sheet's mean — `SkyContent::mCloudMean`, measured at load, with
  `CLOUD_THICKNESS_MAX` bounding the ratio.

  Six of the ten weathers reach a sheet the archives hold, and `tx_sky_overcast`, `_rainy` and
  `_thunder` carry 255 alpha in every texel, so the alpha alone drew those three as one flat colour
  across the whole sky. Their alpha-weighted mean luminances are 0.268, 0.283 and 0.357, against
  clear 0.435, cloudy 0.552 and foggy 0.639. On a clean patch of noon sky the display standard
  deviation goes 0.0016 to 0.0212 for overcast and 0.0121 to 0.0231 for rain, while the patch mean
  moves under two per cent — shape where there was none, and the level left where it was.

- **A cloud is darker than the sky it covers**, and it is now. Plane-parallel theory puts a deck's
  transmission at 0.2 to 0.3; `CLOUD_TRANSMISSION` is 0.25, and thin cloud is not dragged down with
  it because how much sky a wisp replaces at all is its own alpha.

  Measured, linear, at Seyda Neen. A clear midnight: the sky over the deck delivers 0.0144, so the
  deck radiates 0.0036 in its own shadow and 0.0053 where the moons reach it — a dark shape against
  the sky it hides, which is what a night cloud is. A clear noon: 0.0705 shadowed against 0.684 lit,
  because the sun is nine tenths of what reaches the layer. An overcast noon: 0.0687 against 0.293,
  the weather's own glare having taken most of the sun before it arrives.
- **The layer keeps the sun after the ground has lost it**, and crosses less air on the way, which is
  why a sunset cloud is gold rather than black.
- **And it casts**, at a coarse mip, letting about a quarter through.

**The trap this fork has and that one did not.** `CloudShell::mCurvature` is `k · h` fitted to
Morrowind's cap, and it comes to 0.0575. Read as a world radius that is `h / R = 0.128`, where the
Earth's is 7.8e-5 — so `sqrt(2h/R)` off it gives a sunset dip of **27 degrees** rather than the
fraction of one a real layer has. **The shell is a shape fit and not a planet.**

**What both of the steps below turn out to want is one number: how high the layer is.** The mesh
states its height in *tiles* of its own sheet — `CloudShell::mTiles`, 0.711 — and nothing anywhere
states a metre. Give it one and three things follow at once: a tile is `altitude / 0.711` across the
world, which is what anchors the deck to the ground under it; the layer sees the sun until it is
`sqrt(2 * altitude / R)` under the ground's horizon, against the Earth's own radius rather than the
mesh's fit; and a cloud's shadow is the size of the cloud, so the same division sets how large a
shadow reads. It is the one chosen number in the layer, and `Constants::UnitsPerMeter` is what it is
spelled in.

At five hundred metres — the reference's own choice, made for the shadows — a tile is 703 m, the
deck's outermost ring stands 1.76 km out and so ends 16 degrees above the horizon, and the dip is
0.72 degrees.

**And the engine's sunset is a clock, not a horizon**, which is the finding that decides the second
step's shape. `Sky::sunShareAt` ramps on the hour: one until `mDayEnd`, then `1 - fade^2` to nothing
at `mNightStart` — 18:00 to 20:00 on the shipped fallbacks. `Sky::sunAt` puts the disc level with
the horizon at exactly `mNightStart`, so the ramp and the geometry agree there and nowhere else is
an elevation asked for. So a deck cannot be handed a lower horizon; it has to be handed an *earlier
hour*.

Morrowind's disc climbs `sSwing - |east|` over a horizontal swing of `sqrt(sSwing^2 + sNorthing^2)`,
so near either end of the day it moves `2 * 400 / 406.97` radians per unit of orbit, and orbit is
linear in the hour across a fourteen-hour day: **8.04 degrees an hour**. The 0.72-degree dip is
therefore 5.4 game minutes of clock. Read against the two-hour dusk that is a shift of 4.5%, and at
the instant the ground's sun goes out the deck still holds 8.7% of it — a gold rim on the cloud after
the ground has gone dark, fading over the next five minutes. Modest, and it is the whole effect.

**The air mass is not part of it, and that answers the question this plan has been carrying.**
`Sun_Sunset_Color` and `Sun_Disc_Sunset_Color` are the content's own reddening and they are keyed on
the hour, so `Rtx::airTransmittance` over the top of them is the same sunset stated twice. The deck's
dusk differs from the ground's in *when it ends*, not in what colour it is. The moons want the same
offset for the same reason and are worth less: `MoonPlacement::mAlpha` is the engine's own fade near
the horizon, and a deck holding a moon five minutes longer is not a picture anybody will see.

**And the mesh's own crossing stays**, which is worth stating because the obvious move is to replace
it. A ray against a real sphere at 500 m and the Earth's radius differs from a flat layer by 0.05% at
16 degrees of elevation and less above it — the layer's own horizon is 80 km away and the deck's
outermost ring is 1.76 km, so the deck never reaches the band where a planet's curve is worth
anything. `CloudShell::mCurvature` meanwhile pulls the crossing in by 32% at that same ring. It is
Morrowind's own convergence and replacing it with a planet would stretch the sheet by half at the rim
in exchange for four hundredths of a per cent of geometry.

**The deck's own sunset is in.** `Rtx::sunShareAloft` reads `Sky::sunShareAt` at the clock the deck
is on, `SkyReading::mSunShareAloft` carries it to `makeSkylight`, and `Skylight::mSunAloft` is the sun
a layer above the ground gets. One thing the writing turned up that the investigation had not: **the
deck's day is the ground's widened at both ends rather than moved.** A layer that sees the sun lower
catches the sunrise early as well, and a single shift of the clock hands the morning *less* sun than
the ground itself gets — so it is the larger of the ramp read either side.

**Steps.** What is left:

1. **The deck anchored to the world, and its shadow on it.** These are one step: an anchor with no
   shadow only changes how the sky moves, and a shadow without one would follow the player rather
   than lie under the cloud that casts it.

   The sheet is addressed from where the eye stands rather than from where it looks, which the
   altitude makes possible — a tile is `altitude / CloudShell::mTiles` across the world. The fade
   rings stay where they are, because they are the mesh's own and are measured from the eye.

   The shadow is that sheet sampled where the ray from a shading point to each light above it crosses
   the layer, at a coarse mip, through Beer-Lambert rather than a mix — the reference measured a mix
   saturating 48.5% of the clear sheet at one flat value, where an exponential never flattens. A deck
   lets about a quarter through, which is `CLOUD_TRANSMISSION` read from the other side.

   **The sheets ship no mip chain**, which the shadow needs and `cloudDeck`'s own sampling would
   like: `tx_sky_*.dds` is a single 512-square level, and a shadow read at level zero is a shadow
   with a cloud's own alpha edge on it rather than a shadow's. Building one at load is part of this.

   **And it changes the sky's own behaviour**: the deck stops travelling with the camera. That is
   right — a rasterizer's sky is a dome around the eye because it is drawn as one, which is a
   workaround and not a decision about the world — and it is the thing to look at hardest here,
   because a cloud that crosses a 703-metre tile as a player walks is a thing Morrowind never did.

---

## 2. What terminates a path is what the open sky delivers

`mAmbient` used to be six times the dome it stood for a bounce of, so a surface in a crevice was lit
more than the open ground beside it. `SkyBudget` closed that: the sky now delivers the weather's
ambient exactly, and `pathEnd` is that same figure.

**Which leaves the other half of it.** A bounce ray that hits something is now shaded as though that
something saw the whole sky. Nothing accounts for how enclosed it is, so the term that is meant to
stand for the bounces nobody traces cannot darken a hole.

**How large it is, measured against the shape of the estimator rather than guessed.** The first level
is already occluded and always was — `bounceLight` traces a real hemisphere, and a ray that hits
something is shaded rather than handed the sky, which is the reference's "ambient occlusion is not a
separate effect, it is the same integral, sampled". What is flat is one level down. That term reaches
the eye through two albedo multiplies, so a hole is over-bright by roughly a quarter of the ambient
and only in the bounce channel. **And indoors it is not wrong at all**: `mAmbient` is then the cell's
own `AMBI`, which is the game's answer for that room. What is left is an exterior hollow — under a
bridge, in a doorway, at a cave mouth — lit as though the sky reached it.

**`sprites.glsl` is the sharper half of the same defect**, and it is first-order rather than second:
a particle is lit by `pathEnd` directly, with no bounce over it, so rain and smoke at an exterior
cave mouth carry the open sky at full strength.

**Three callers, one missing quantity.** `bounceLight`'s second hit, `waterRay`'s, and every sprite
all ask `pathEnd(position)` and none of them can say how much sky that position sees. So the fix is
not a change to `pathEnd`'s arithmetic; it is giving the renderer something that can answer that
question for an arbitrary world point.

Two shapes for it, and they are not the same size:

- **One visibility ray at the bounce hit.** A cosine-weighted ray from the second hit, traced for the
  hit and not for the light, scaling the term by whether it escaped. It costs one ray on every pixel
  whose bounce hit something — free outdoors, where most escape, and a doubling of the bounce's rays
  indoors, where none do. One binary sample is noisy, but it multiplies a term that is already one
  noisy sample and rides the same filter. **It does nothing for the sprites**, which have no ray to
  hang it off.

- **A sky-visibility field in world space**, which is the durable answer and serves all three. The
  light grid already bins the world for the lamps; a scalar per cell saying how much sky that cell
  sees is the same structure carrying one more number, and unlike the ray it is temporally stable and
  costs a lookup rather than a traversal. It is a feature rather than a fix, and it is what a probe
  volume would grow out of — `probe.comp` is a memory-access diagnostic and not a light probe, so
  there is nothing to build on yet.

Lowest priority of them either way. It is worth revisiting after the deck's shadow, because a deck
that shadows the world is the same question — what stands between a point and the sky — asked from
the other side.

---

## Order

Step 1 is the last of the sky, and one of its own steps is left. It is the one to look at hardest,
because it changes how the sky moves rather than how it is lit. The sheets' own shape, the light on
them and the deck's own sunset are all in place now.

Step 2 stays open, and its second shape wants a feature this renderer does not have.

**And one thing the star pass leaves behind.** It costs 0.31 ms at 3840 by 2160 and only at night —
1.1% of a 28 ms frame — and nearly all of that is the unwrap's two inverse trigonometric calls over
every sky pixel, not the image traffic the fold into the display pass already saved. If it ever has
to come down, that is where it is.

## What must still hold at each step

- `openmw-rtxtool shot --hour 12` changes in step 1 only where cloud is drawn.
- The sky's total delivered light equals the weather's ambient at night, whatever the layers say.
- `components-tests` and `openmw-tests` pass, and the formatting check is clean.
- No new allocation on the frame path. Everything read off a mesh or an image is read at load.
