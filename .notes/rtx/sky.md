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
| the cloud deck | `cloudDeck` | `cloudShadow`, over the sun and the moons |

The compositing order is settled, the moons rise through the air rather than being switched off, and
the star field is drawn by the display pass at the resolution it is shown at — the three upscale
settings now put its brightest pixel at 0.980 apiece where they used to run from 0.973 down to 0.864.
The second column is filled now: every layer lights as well as draws.

**What the sky delivers is stated once, and every layer that lights comes out of it.** `skyFill`
holds the sky to what `Ambient_<weather>_Night_Color` says a night is worth, and it now subtracts the
dome and the night's sheets alike — so the stars started lighting and the night stayed where it was.
The deck is the last layer outside that rule.

Measured at a clear midnight, linear luminance: the weather's ambient is **0.0168**, the dome is
**0.0030**, so the fill is **0.0138**. On day 0 the moons add **0.0125** perpendicular, **0.0113**
onto flat ground.

---

## 1. The cloud deck — done

The deck is lit rather than painted, it keeps the sun after the ground has lost it, and it shadows
the world under it. What each piece cost and why it is shaped that way:

- **The sheet gives shape and the light gives colour.** `SkyContent::mCloudMean` at load, the texel's
  luminance against it in the shader, `CLOUD_THICKNESS_MAX` where that ratio reaches a cloud in full
  sun. Three of the six sheets the game reaches are 255 alpha in every texel, so for those the paint
  is the only shape there is: the display standard deviation on a clean patch of noon sky went 0.0016
  to 0.0212 for overcast.

- **A cloud is darker than the sky it covers.** `CLOUD_TRANSMISSION` is 0.25, and `Rtx::deckLight`
  spreads the sky, the moons and the sun over the underside. At a clear midnight the sky over the
  deck delivers 0.0144 and the deck radiates 0.0036 shadowed against 0.0053 lit; at a clear noon,
  0.0705 against 0.684.

- **The layer keeps the sun after the ground has lost it.** `Rtx::sunShareAloft` — an earlier hour
  and not a lower horizon, because the engine's sunset is a clock. 0.718 degrees of dip at 8.04
  degrees an hour is 5.35 game minutes, and the deck still holds 8.7% of the sun at the instant the
  ground has none. The air mass stays out: the content's `Sun_Sunset_Color` is the same sunset.

- **And it casts.** `cloudShadow`, Beer-Lambert over the alpha *above the sheet's own mean* — the
  content has already dimmed the sun per weather, and taking the whole alpha would state that twice
  and hardest where it is most wrong, since an overcast sheet is uniform and would come out as one
  flat second dimming.

**Two things the writing turned up that the investigation had not.**

The deck's day is the ground's **widened at both ends** rather than moved: a layer that sees the sun
lower catches the sunrise early too, so a single shift of the clock hands the morning less sun than
the ground itself gets.

And **the mesh's bowl and a shadow cannot share one geometry**. Morrowind's cap is a strong
compression — it lets the deck reach 4.7 degrees above the horizon where a flat layer stops at 15.9 —
and it is centred on the viewer by construction, so no world-anchored copy of it exists. The eye
keeps the bowl, because dropping it would take a third of the sky away; the shadow takes the honest
flat crossing. Straight overhead the two meet exactly and read the same texel, at 45 degrees they are
5% apart, and at 14 degrees half again — which is a cloud low in the sky whose shadow is kilometres
off and out of any frame the two could be compared in.

**And one number to come back to.** The shadow is exact — a floor under a uniform sheet darkens by
`exp(-4)` to the neper — and subtle in the frame, because a cloud's shadow is the size of the cloud
and one feature of the sheet is about 140 m against a visible landscape of a few hundred. That is
§8.57's own finding, and the tile's width follows `Rtx::sCloudAltitude`, so it is one number that
moves it.

**The mip chain turned out not to be needed.** The sheets ship one 512-square level, and the arithmetic
says that is enough: the sun's half-degree blurs a shadow by four metres at this altitude against a
texel's 1.4, so a level-zero fetch is already softer than the sheet, not sharper. The reference needed
mips because it read the sheet per screen pixel through a cone; this reads it once per shading point,
over a footprint smaller than a texel.

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

Step 1 is done, and with it the sky. What changed last and is worth looking at hardest is that the
deck no longer travels with the camera: it is addressed from where the eye stands, so a cloud crosses
a 703-metre tile as a player walks, which is a thing Morrowind never did.

Step 2 stays open, and its second shape wants a feature this renderer does not have.

**And one thing the star pass leaves behind.** It costs 0.31 ms at 3840 by 2160 and only at night —
1.1% of a 28 ms frame — and nearly all of that is the unwrap's two inverse trigonometric calls over
every sky pixel, not the image traffic the fold into the display pass already saved. If it ever has
to come down, that is where it is.

## What must still hold

- The sky's total delivered light equals the weather's ambient at night, whatever the layers say.
- `components-tests` and `openmw-tests` pass, and the formatting check is clean.
- No new allocation on the frame path. Everything read off a mesh or an image is read at load.
