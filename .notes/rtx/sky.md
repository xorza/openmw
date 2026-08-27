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
fraction of one a real layer has. **The shell is a shape fit and not a planet.** Whatever decides how
long the deck keeps the sun has to come from somewhere else.

**Steps.** Land them in this order, checking with `shot` after each:

1. The deck's own horizon and its own air mass, so it keeps the sun after the ground has lost it.
   Day and dusk. The trap above is this step's.
2. The deck's shadow on the world.

---

## 2. What terminates a path is what the open sky delivers

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

Step 1 is the last of the sky, and two of its own steps are left. It is the one that can regress a
day that has just been tuned, and both the sheets' own shape and the light on them are in place now.

Step 2 stays open.

**And one thing the star pass leaves behind.** It costs 0.31 ms at 3840 by 2160 and only at night —
1.1% of a 28 ms frame — and nearly all of that is the unwrap's two inverse trigonometric calls over
every sky pixel, not the image traffic the fold into the display pass already saved. If it ever has
to come down, that is where it is.

**And one question this leaves for step 2.** `Rtx::airTransmittance` is the moons' now and not the
sun's, because `Sun_Disc_Sunset_Color` and `sunShareAt` already redden and ramp that disc between
them. A lit cloud deck needs the sun's air mass for its own reasons, and whoever writes it has to
settle whether the content's sunset and the air's are the same sunset stated twice.

## What must still hold at each step

- `openmw-rtxtool shot --hour 12` changes in step 1 only where cloud is drawn.
- The sky's total delivered light equals the weather's ambient at night, whatever the layers say.
- `components-tests` and `openmw-tests` pass, and the formatting check is clean.
- No new allocation on the frame path. Everything read off a mesh or an image is read at load.
