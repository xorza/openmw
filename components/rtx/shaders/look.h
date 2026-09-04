// `#pragma once` everywhere else in this tree, and an include guard here for the reason
// `portable.h` gives.
#ifndef OPENMW_COMPONENTS_RTX_SHADERS_LOOK_H
#define OPENMW_COMPONENTS_RTX_SHADERS_LOOK_H

#include "portable.h"
#include "scene.h"

// Every number that decides how the picture looks, in one file, so that tuning it is reading one
// file rather than remembering which of thirty holds the dial.
//
// **What belongs here is a number somebody turns to change the frame.** Its source does not decide
// that: a taste dial nobody measured, a real constant that fixes a colour, and a sample count that
// fixes how clean the frame is are all things a person reaches for when the picture is wrong. What
// does not belong is everything the picture does not depend on — units and maths, buffer and grid
// sizes, workgroups, traversal masks, enumerations, and the biases and guards that keep the
// arithmetic honest. Those stayed where they are, beside the code that has to agree with them.
//
// **Three dials stayed behind, and each says why where it stands.** `FOG_CHURN` and `FOG_TURN` in
// `fog.glsl` are one entry per scale of the fog's fractal, written in GLSL's own array syntax that
// the host cannot read — and this file is shared verbatim with C++ so that a test can compute
// against what the shader used. `WATER_CAUSTIC_MAX` in `scene.h` clips the brightest filament, and
// the polynomial beneath it is fitted against that clip: the two are one statement, and separating
// them would leave a dial whose fit lives somewhere else.
//
// The order is the order the light travels in reverse, from the eye outward: what the frame is
// exposed and graded through, then the sky that lights it, the surfaces it lands on, the bounce off
// them, the air in front of all of it, the water, the sprites, and last how many frames and samples
// go into settling the result.

#ifdef RTX_HOST

#include <osg/Vec3f>

namespace Rtx::Shaders
{
    using vec3 = osg::Vec3f;

#endif

    /// Darkest luminance the histogram resolves, as a power of two. About a thousandth of mid grey,
    /// which is below anything a lit surface reaches and well under an unlit interior.
    const float MIN_LOG_LUMINANCE = -10.0f;

    /// Brightest, as a power of two. Sixty-four times mid grey covers a flame seen directly.
    const float MAX_LOG_LUMINANCE = 6.0f;

    /// Where a pixel stops being binned and starts being counted as black.
    ///
    /// Without it the dark areas of an interior pile into the lowest bin and drag the average down
    /// to meet them, and the exposure opens until the few lit surfaces are white.
    const float EXPOSURE_BLACK = 0.0001f;

    /// How much the curve takes off the darkest channel once it has any to take. Khronos's own.
    const float TONE_SHADOW_OFFSET = 0.04f;

    /// Where `toneMap` stops leaving a colour alone and starts bringing it down.
    ///
    /// Khronos's own, less the shadow offset, which it has already taken off by then.
    const float TONE_COMPRESSION_START = 0.8f - TONE_SHADOW_OFFSET;

    /// How far a compressed colour is carried toward white. Khronos's own.
    const float TONE_DESATURATION = 0.15f;

    /// How much of each coarser level survives into the one above it.
    ///
    /// **The pyramid is mixed rather than summed**, which is what keeps the total independent of
    /// how many levels there are: `mix(finer, coarser, this)` at every step, so a frame that built
    /// one level fewer is a narrower bloom and not a dimmer one. Higher is a wider, softer veil.
    const float BLOOM_SCATTER = 0.75f;

    /// How much of the pyramid is left in the picture.
    ///
    /// **No threshold anywhere, which is why this is small.** A lens spreads every photon that
    /// reaches it and not only the bright ones, so the whole frame is blurred and mixed back at a
    /// few per cent — where a threshold makes bloom arrive as an object crosses a brightness nobody
    /// can see, and takes the veil off everything under it. What makes a Morrowind sun read as a
    /// sun is that its disc is a hundred times the median of the frame around it, not that anything
    /// selected it.
    const float BLOOM_STRENGTH = 0.05f;

    /// Irradiance of the sun against the sky it is set in.
    ///
    /// Not a physical figure: exposure absorbs any overall scale, so what matters is the ratio
    /// between the direct sun and the sky, roughly five to one on a clear day on a surface facing
    /// it. Shared with the shader because everything else on this scale is measured against it.
    const float DAYLIGHT = 8.0f;

    /// Angular radius of the sun, in radians — a disc about half a degree across.
    ///
    /// The real figure, because there is only one right answer and nothing about this renderer wants
    /// a different sun. It decides how wide the disc in the sky is drawn, and with it how wide the
    /// glitter path on water is: the two are the same number seen twice, one directly and one in a
    /// mirror, and they cannot be allowed to disagree.
    const float SUN_ANGULAR_RADIUS = 0.004654f;

    /// Angular radius of the cone a sun shadow ray is drawn from, in radians: two degrees.
    ///
    /// **Wider than the disc, on purpose, and the disc is not moved with it.** A shadow cast by the
    /// real half degree has a penumbra a centimetre wide on a wall two metres behind what casts it,
    /// which on a screen is a hard edge — and Morrowind never had one. The game's shadows are maps
    /// at a thousand texels over eight thousand units, filtered, and so soft at every distance; the
    /// one the tracer draws was judged too sharp beside them. Two degrees puts a penumbra twenty
    /// units wide on a wall three hundred units behind its caster, which is about what the maps
    /// drew.
    ///
    /// This is a choice about the look and not a measurement, which is why it is a constant of
    /// its own: the disc in the sky and the glitter path on the water stay at the real size, since
    /// those are the sun seen and a sun seen wider is a different sun. A sun seen through haze does
    /// widen its own shadows — the aureole a hazy sky throws round it is a few degrees across — and
    /// if this ever wants to follow the weather, that is the model to follow it with.
    const float SUN_SHADOW_RADIUS = 0.034907f;

    /// The most radiance the sun's disc is drawn with.
    ///
    /// **A ceiling for a temporal history, not for a picture.** The sun's disc is drawn at its
    /// irradiance spread over its own solid angle, which at noon is `8 / (pi * 0.004654^2)` — a
    /// hundred and seventeen thousand. Nothing downstream can use it: the dimmest exposure the
    /// renderer will choose is 0.05, so a radiance of 20 is already the top of the display range at
    /// every exposure it can pick. What the number does reach is the upscaler, which reconstructs
    /// from several frames of linear radiance and has to hold that value in a history — and a
    /// neighbourhood five orders of magnitude out of range is one it clears slowly, which is a
    /// blown pixel that stays blown for seconds after the sun has left the frame.
    ///
    /// **A thousand, because a glint is the dimmest thing this can reach.** Water reflects `WATER_F0`
    /// of what it faces at normal incidence, so a source has to survive a factor of 0.02 and still
    /// clear the display's top: `20 / 0.02` is the smallest ceiling that leaves every white pixel
    /// white. It is a hundred and eighteen times below where the disc sits.
    ///
    /// **The disc alone, because it is the only thing in the sky that can reach a ceiling at all.**
    /// A moon's face is held at 0.18, a star at the same, and the dome's own glow is a decoded
    /// weather colour — every one of them three orders below this.
    const float MAX_SUN_RADIANCE = 1000.0f;

    /// What a moon's own texels are worth as radiance.
    ///
    /// **Pinned, and not by taste.** A real full moon is a 640,000th of the sun and there is no scale
    /// this renderer could put both on, so the number has to be chosen — and what chooses it is that
    /// a moon bright enough to blow all three channels is a white disc whatever colour it was given,
    /// which throws away the only reason to draw Masser rather than a bright dot. `tx_masser_full`'s
    /// mean opaque texel is 0.0332 in red, and this is what takes that to 0.18: the most red a moon
    /// can be and still be a moon.
    ///
    /// It multiplies the portrait where one is loaded and the portrait's mean where none is, so the
    /// two paths are the same brightness and only the detail differs.
    const float MOON_RADIANCE = 5.4217f;

    /// How much of the sunlight falling on Masser comes back off it: its geometric albedo, which is
    /// what a body sends back at opposition against what a perfectly diffusing disc of the same size
    /// would.
    ///
    /// **The Moon's own, because there is one right answer and Morrowind's moons are rock** — the
    /// argument `SUN_ANGULAR_RADIUS` makes. What it buys is that moonlight stops being a level
    /// somebody picked. A disc of geometric albedo `p` and half-angle `t` under irradiance `E`
    /// delivers `E * p * sin(t)^2` to a surface facing it, and every term of that is already here:
    /// `DAYLIGHT` is the sun, and `Moons_<name>_Size` is the angle.
    ///
    /// **And it is the size of Morrowind's moons that makes the physics usable.** Earth's is half a
    /// degree across, so the same formula puts real moonlight at a 407,000th of sunlight — a figure
    /// no frame here could carry beside a noon. Masser is eleven degrees across at OpenMW's own
    /// `Moons_Masser_Size` and nineteen at the ini's, which is four hundred to thirteen hundred
    /// times the sky and puts it between an 858th of the sun and a 299th. A Morrowind night is lit
    /// by its moons because its moons are enormous, and nothing has to be invented to say so.
    ///
    /// **Masser's alone, because the two moons do not reflect the same.** `tintOf` normalises both
    /// portraits on Masser's luminance, so Secunda carries this times the 2.54 its own paler face
    /// says it is worth. And this is not `MOON_RADIANCE`: that one is pinned by where the tone curve
    /// stops keeping colour, so a light read out of it would put a night at a thousandth of a day.
    const float MOON_ALBEDO = 0.12f;

    /// What a texel of the star field is worth as radiance.
    ///
    /// **Morrowind puts a star at the top of the display range.** `paintAtmosphereNight` hands the
    /// sheet's own texel to a `(SRC_ALPHA, ONE_MINUS_SRC_ALPHA)` blend, so a white texel at full
    /// alpha lands on the frame buffer at one, over a night sky of 9, 10, 11 out of 255. That is
    /// also how the engine gets crisp stars out of a bilinear sampler: every pixel of the blob it
    /// spreads a star over saturates, and what shows is a hard dot. Nothing here clips, so the level
    /// has to put a star where the content puts it rather than rely on the ceiling to do it.
    ///
    /// **Measured through the path the game ships**, which is the only place the answer is real: at
    /// 1920 by 1080 under `--upscale quality`, on a clear midnight, this puts the brightest star at
    /// 0.914 of the display range where 0.18 put it at 0.627. The fifth still missing is Ray
    /// Reconstruction's — the same frame drawn without it reaches 0.973.
    ///
    /// **A star is still never brighter than a full moon**, which is the rule and was the old level's
    /// whole derivation — but it was read against Masser's *mean* texel, and a portrait's peak is
    /// several times its mean. Masser's brightest pixel measures 0.932 in the same configuration, so
    /// the rule holds with the bound stated where it belongs.
    ///
    /// **It reaches what is drawn and never what lights.** A bounce that escapes takes `skyGlow`,
    /// which carries the sheets as one mean — `NightSky::mGlow` — so raising this raises that too and
    /// `skyFill` takes it back out of the weather's own ambient. The night's light does not move.
    const float STAR_RADIANCE = 0.45f;

    /// What a texel of the three nebulae is worth as radiance.
    ///
    /// **A wash over the sky it washes.** Most of a Morrowind night's colour is in these rather than
    /// in the stars: two of the three reach past a radian, so what they do is tint half the sky at a
    /// time.
    ///
    /// **This level was set against the wrong average and the note said so.** It read
    /// `tx_stars_nebula` at 0.052 and had this put that at 0.003, which is what `Sky_Night_Color`
    /// decodes to — but 0.052 is the sheet's colour with its alpha ignored, and `skyPatches` draws
    /// `rgb * a`. That mean measures 0.00199, so a nebula's average comes out at 1.2e-4 against the
    /// night sky's 0.003: a twentieth of it rather than a match. Whether a nebula should read as the
    /// sky it lies over is a question about the picture, and the number here answers it as a wash.
    const float NEBULA_RADIANCE = 0.06f;

    /// What the engine paints the second row of its cloud mesh with.
    ///
    /// `ModVertexAlphaVisitor::Clouds`'s own 64 over 255, and the only number in the deck's fade
    /// that is not a radius read off the mesh. `CloudShell::mRings` carries where it applies.
    const float CLOUD_RING_ALPHA = 0.25098f;

    /// How far above its own mean a texel of a cloud sheet reads as a cloud in full sunlight.
    ///
    /// **What the sheet's paint is a ratio against.** `CloudDeck::mMean` divides a texel's luminance
    /// by what its sheet averages, and this is where that ratio reaches one — so a texel at the mean
    /// is a cloud half way between its own shadow and its lit face, and one at twice the mean is
    /// lit through.
    ///
    /// **Two rather than one, because one is where the shape goes.** Half a sheet's texels lie above
    /// its mean by definition, so a scale that saturated there would flatten half of every sheet
    /// onto one value. Measured over the six sheets the shipped fallbacks reach, the 99th percentile
    /// of the ratio runs 1.10 to 1.63 and the brightest texel of any of them is 2.03 — so at two
    /// almost nothing saturates and the whole painting carries.
    const float CLOUD_THICKNESS_MAX = 2.0f;

    /// How much of the light landing on a cloud deck leaves the underside of it.
    ///
    /// **A cloud is darker than the sky it covers, and this is the whole of why.** Water droplets
    /// scatter nearly everything that reaches them, but most of it leaves *upward*: plane-parallel
    /// theory puts a deck's transmission at 0.2 to 0.3. At 0.9 — the figure a "lit from the whole
    /// hemisphere" argument suggests, which is true of the irradiance arriving and silent about what
    /// leaves — a night deck is 90% of the sky it hides and so cannot be seen at all.
    ///
    /// Thin cloud is not dragged down with it: how much sky a wisp replaces at all is its own alpha,
    /// which is `cloudDeck`'s coverage and not this.
    const float CLOUD_TRANSMISSION = 0.25f;

    /// How dark a cloud's shadow is, in nepers per unit of alpha over the sheet's own mean.
    ///
    /// **Over the mean and not over nothing, because the content has already dimmed the sun.**
    /// Morrowind gives every weather its own `Sun_*_Color`: clear's is 255, 252, 238 and overcast's
    /// is 163, 169, 183, so the average cloud is in the number before this renderer touches it. A
    /// shadow that darkened by the whole of the alpha would state the weather twice — and worst
    /// where it is most wrong, since `tx_sky_overcast` is 255 alpha in every texel and would come
    /// out as one flat second dimming with no shape in it at all. Subtracting the sheet's own mean
    /// leaves the level where the content put it and adds only the pattern.
    ///
    /// **And a cloud never brightens the sun**, which the `max` at nought is: a gap in the sheet is
    /// an open sky and not a lens.
    ///
    /// Four is the reference implementation's own figure and the one number in the layer chosen
    /// rather than derived. Clear weather's sheet is cirrus, which in life casts almost nothing, and
    /// a shadow that cannot be seen is not worth tracing.
    const float CLOUD_SHADOW_DEPTH = 4.0f;

    /// How far the estimate may reach either way, which is also how a map is stored.
    ///
    /// **A map on the device is a sixteen-bit unorm of `(value - floor) / (ceiling - floor)`**, so
    /// the whole of the format's range is spent on the values a map can hold — a step of one part
    /// in forty thousand — and the neutral map, one everywhere, lands on exactly a third: 21845 of
    /// 65535, which the decode carries back to exactly one. `Rtx::ShadingMap` says why the bounds
    /// are what they are, and `Rtx::encodeShading` is the one statement of the encode.
    const float SHADING_FLOOR = 0.5f;
    const float SHADING_CEILING = 2.0f;

    /// Untextured surfaces are mid-grey rather than black, so a missing texture reads as missing rather
    /// than as shadow.
    const vec3 NO_TEXTURE_ALBEDO = vec3(0.5f, 0.5f, 0.5f);

    /// What an emissive of one is worth on screen.
    ///
    /// **The original's scale is not this renderer's.** There a fully lit surface reached one and an
    /// emissive of one matched it; here the direct sun is `DAYLIGHT`, so the same number has to be
    /// carried across or a glow that read as bright becomes a rounding error.
    ///
    /// **What sets it is what a night frame shows.** A night's exposure is metered off a dark scene,
    /// so a surface held high enough washes to white and loses the pattern on it — a glowing
    /// mushroom cap reading as a blob rather than as a mushroom. `FLAME_INTENSITY` says the same of
    /// a sprite and answers it by deriving a fully lit white card, `DAYLIGHT / pi`; this sits about
    /// three times that, chosen against rendered frames rather than derived, because a material's
    /// one and a sprite's one are two content conventions and not one.
    ///
    /// **A glow lights nothing, and this is the whole of what it does.** A glowing surface used to
    /// earn a lamp of its own beside what it shows. Measured with those lamps switched off, three
    /// frames moved by 0.5 %, 1.2 % and 2.9 % — a mushroom lost the warm ring on the ground under
    /// its cap and kept everything else, and a guild lit by its own sconces was hard to tell apart.
    /// What the lamps cost was a light in the grid for every glowing shape in the world, 474 of them
    /// at Seyda Neen and 248 at Ald-Ruhn, and they were the reach that drove `LightGrid` to coarsen
    /// its cell. Morrowind lights what it means to light with a `LIGH` record.
    const float EMISSIVE_INTENSITY = 8.0f;

    /// What light on the far side of a leaf is worth to the side being looked at, against the same
    /// light on the near side.
    ///
    /// **A leaf is a sheet with a mask, and it is the one surface in the game lit from behind.**
    /// The content marks it exactly — a card doubled for its back, under an alpha property — and a
    /// real leaf passes about half of what it reflects, so a canopy against the sun glows through
    /// rather than going black. The same albedo on either side: what colours the light through a
    /// leaf is the leaf. Half, rather than the tenth a leaf transmits absolutely, because the term
    /// scales the surface's own diffuse response and not the sun; the reference implementation
    /// measured backlit foliage a quarter brighter at this value and no noisier.
    ///
    /// **The far side's light is the term, and the shadow it would have cast stays whole.** What a
    /// leaf lets through to the ground under it arrives by the bounce that lands on the leaf's
    /// underside and gathers the sun there — so a shadow ray thinning itself through the leaf's body
    /// as well would deliver the same light twice.
    const float SHEET_TRANSMISSION = 0.5f;

    /// What share of indirect hits out of doors are lit at all, the rest paying by weight.
    ///
    /// **The two rays a bounce hit spends on direct light are the dimmest pair in the frame.** A sun ray
    /// and a lamp ray are traced there to light a term nothing resolves on its own — the moons are
    /// already refused that path for exactly this reason, and the ambient ray beside them is already
    /// drawn at `AMBIENT_EXTERIOR_RATE`.
    ///
    /// **Drawn and divided, so the estimate is unbiased by construction** rather than a guess at what
    /// the unlit half would have said. Two frames of a thousand samples each agree exactly, which is how
    /// that was checked. What it hands the filter is variance in the channel Ray Reconstruction
    /// demodulates and filters hardest, which is why it is judged on a moving camera rather than a still.
    ///
    /// **Half the rays are not half the time, and the gap is worth knowing.** Removing the pair outright
    /// takes the trace from 4.30 ms to 3.59 at the ship at Seyda Neen and from 3.03 to 2.77 over
    /// Balmora; a half rate, measured interleaved against its own baseline, takes 4.35 to 4.22 and 3.13
    /// to 3.07 — a fifth of what the gut says, where `AMBIENT_EXTERIOR_RATE` claimed half of its own.
    /// The rays here are short: a lane that skips one does not release the warp, which runs on until the
    /// lanes that kept theirs are done, and a hashed draw leaves no warp with thirty-two skipping lanes.
    /// The ambient ray runs to `mFar` and is nearly all empty traversal, so halving those halves what
    /// the device does whatever the warp is doing. **A rate is worth what the ray it drops is long.**
    ///
    /// **Out of doors only, and that is not caution about the arithmetic.** A room's indirect light *is*
    /// its lamps seen once off a wall — the interior ceiling is the larger of the two, 4.21 ms to 3.58 in
    /// the Guild of Mages — so rating it there halves the samples of the term that carries the room,
    /// where outside the sun has already lit everything the bounce lands on. Every interior view renders
    /// bit-identically under this, which `verify` says.
    const float INDIRECT_LIGHT_RATE = 0.5f;

    /// How fast a bounce ray's cone widens, against a primary ray's.
    ///
    /// A diffuse bounce spreads over the whole hemisphere, and what the indirect term wants from a
    /// texture is its *average* rather than any texel of it — so the cone is opened to about a radian,
    /// which reads the coarse mips a bounce should see without collapsing every one to the top level.
    const float BOUNCE_SPREAD = 1.0f;

    /// How far out of doors a surface traces its own bounce, in world units.
    ///
    /// **Beyond it the hemisphere is not traced and the escape is taken as though nothing stood in the
    /// way.** The far half of an exterior is thousands of pixels whose bounce ray leaves a mountainside,
    /// crosses the whole acceleration structure and mostly finds sky anyway — and whose indirect term
    /// the upscaler then averages flat, because a pixel that far away covers a hillside. What the ray
    /// was proving is that nothing was there, at the price of the longest traversal in the frame.
    ///
    /// One cell, which is the distance Morrowind itself builds a world in. Nearer than that a bounce is
    /// what fills a doorway, an arch and the shaded side of a house, and every one of those is inside
    /// the cell the camera stands in.
    ///
    /// **Biased, unlike the two rates beside it, and it is the bias that makes it worth having.** A draw
    /// and a divide would keep the traversal on half the pixels and the noise on all of them; this stops
    /// the traversal outright, and pays for it in ground lit slightly flatter than it would be. It is
    /// exterior-only for the reason `AMBIENT_EXTERIOR_RATE` is: a room's escape is nothing at all, so a
    /// surface far down a hall would go dark rather than flat.
    ///
    /// **Ground alone, because distance does not say ground and this was let loose on everything.** A
    /// draw about a patch of open hillside reaches the sky whatever stands nearby; the same draw about
    /// a wall spends half of itself on whatever the wall is attached to, and handing that the sky makes
    /// it too bright by the share it should have lost. Vivec is where that showed: a canton is one face
    /// hundreds of units tall running well past the reach, so the sphere cut through the middle of a
    /// building and the seam swept across it as the camera moved. Twenty-three per cent of that view
    /// differed from a frame with every bounce traced, thirteen thousand pixels of it by more than a
    /// twentieth of the display range and the worst by three quarters of it. With the escape asked only
    /// of ground the same view is byte-identical to that frame.
    ///
    /// **What it is worth depends entirely on where the camera stands.** Measured on the `trace` zone at
    /// 1920x1080, three alternations, against a build that traces every bounce: at eye level it is worth
    /// nothing at all — Vivec 4.09 against 4.16 and the ship at Seyda Neen 3.84 against 3.87, both
    /// inside the run-to-run spread — because far ground is crowded into the few rows under the horizon
    /// and the sky above it costs no bounce. A camera looking at a cell from outside it is the other
    /// case, and a hilltop is that camera: the island crossing runs 2.16 ms against 3.02 and the
    /// shoreline 2.66 against 2.98. Letting objects escape as well bought a further 0.58 ms there and
    /// cost the seam above, which is the trade this is the other side of.
    const float BOUNCE_REACH = 8192.0f;

    /// How far a room's fill looks for what is standing over a point, in world units.
    ///
    /// **Two metres, which is the furniture and not the room.** A cell's `AMBI` ambient is a flat stand
    /// in for every bounce the room makes, and it used to reach a point wedged under a pillow exactly as
    /// fully as one in the middle of the floor — so white cloth lit its own contact shadow, and every
    /// crevice next to something pale came out brighter than the surface beside it. What takes the fill
    /// away is what is close enough to be in front of the room rather than part of it, and Morrowind's
    /// rooms are small enough that anything further is a wall.
    const float ROOM_FILL_REACH = 140.0f;

    /// What share of exterior points are asked whether they reach the sky, the rest paying by weight.
    ///
    /// **Out of doors the ambient ray is the expensive one, by two orders of reach.** It runs to
    /// `mFar` where a room's stops at `ROOM_FILL_REACH`, and it is nearly all sky — the traversal is
    /// spent proving that nothing is there. Removing it outright takes the exteriors suite's trace from
    /// 30.9 ms to 26.3; a half of it measured 28.4, so this buys 2.5 ms of a 4.6 ms ceiling and no one
    /// of the seven places came back the wrong way.
    ///
    /// **A half and no further, because a third measured nothing.** Interleaved over the same seven
    /// places, a third came back within 0.01 ms at four of them and 0.09 to 0.12 ms *slower* at the
    /// other three. A rate is a per-lane skip and the ray it skips is a long one, so a warp still runs
    /// until whichever of its thirty-two lanes kept a ray is finished — and at a third, all thirty-two
    /// skipping is a chance in six hundred thousand. What the first halving bought is not on a curve
    /// this can be carried further along.
    ///
    /// **Drawn and divided by the draw, so the estimate is unbiased by construction** rather than a
    /// guess at what the untraced half would have said. What that hands the filter is variance, which
    /// is what the filter is for — and it is the same trade the moon pick makes. Nothing downstream
    /// clamps it: `pathEnd` and a sprite's fill both multiply, so a doubled sample stays worth double.
    ///
    /// **Hashed rather than blue noise, because two callers must not agree.** The bounce and a water
    /// reflection each ask this, and the water's two rays already take separate seeds so that a
    /// reflection and a refraction do not keep one answer between them. A screen-space tile has one
    /// arrangement per channel and would hand every caller the same one.
    ///
    /// The interior ray keeps every point: it is short, and a room is where this term does its
    /// visible work.
    const float AMBIENT_EXTERIOR_RATE = 0.5f;

    /// How far a ray carries fog before whatever is behind it stops mattering.
    ///
    /// Four hundred metres. Past this the transmittance of even the thinnest weather is a rounding
    /// error, and a ray that hit nothing has to stop somewhere.
    const float FOG_REACH = 30000.0f;

    /// The height over the fog's base at which its density falls to `1/e`, in world units.
    ///
    /// Seventy units to the metre, so about thirty-seven of them — a layer deep enough to fill a
    /// valley and still thin out over the hill beside it.
    const float FOG_HEIGHT = 2600.0f;

    /// Where the fog pools when the cell has no water to gather over: sea level outdoors, and close
    /// enough to a floor to serve indoors.
    const float FOG_BASE = 0.0f;

    /// How large one cell of the coarsest scale is, in world units, and so how wide the whole tile is
    /// laid out at that scale.
    ///
    /// **Nine hundred, which is the renderer this is ported from settling the same question twice.**
    /// Its §8.40 made the grain *coarser* — 1,400 to 3,000 units — because structure finer than the
    /// march's step aliased, and got fog whose shape was visible only from a ridge. Its §8.41 found
    /// the diagnosis wrong: sampling finely where the fog is thin buys nothing, because what the eye
    /// reads over a distant hillside is thousands of units of integration and structure at any scale
    /// averages out of it. Fog has visible shape where it is optically thick over a *short* distance,
    /// so the grain came back down to 900 and stayed there.
    ///
    /// **The other half of that finding is free here.** Its fix was *sparse and dense rather than
    /// uniform and thin* — a band clearing more of the volume, with the extinction doubled by hand to
    /// pay for it. `FOG_COVERAGE` divides that back out, so a band that clears more of the ground
    /// thickens what is left by exactly as much, and neither number has to be re-tuned against the
    /// other.
    ///
    /// **What used to stop this from shrinking was aliasing, and the mip chain answers that now.**
    /// The field was hashed at every step, so anything finer than the step between two samples
    /// arrived as noise and the only defence was a grain too coarse to have any. `fogFieldAt` picks a
    /// level from the march's own stride instead, so the field is filtered rather than aliased.
    const float FOG_GRAIN = 900.0f;
    const float FOG_TILE = FOG_GRAIN * float(FOG_FIELD_CELLS);

    /// The standard deviation of the sideways displacement the finer scales are read at, in world units.
    ///
    /// **Domain warping**: rather than adding octaves, the *coordinate* is displaced by a noise of its
    /// own, so shapes stretch and curl instead of staying the roughly round blobs a sum of octaves
    /// gives. Quilez's `fbm(p + w * fbm(p))` at one level — and here it costs nothing at all, because
    /// the coarse scale is fetched anyway and its second channel is a field decorrelated from the first,
    /// so the pair is a vector already in hand. Horizontal only: the vertical shape of this fog is the
    /// height falloff, and warping across it would blur the layer it is meant to have.
    ///
    /// **Half a cell of the coarsest scale, because what a warp does is relative to what it bends.**
    /// A displacement much larger than the feature it moves is not a curl, it is a second draw of the
    /// same field at an unrelated place — so a figure fixed in world units would stop warping and
    /// start scrambling the moment the grain moved. Half is the ratio the renderer this is ported
    /// from settled at: 450 units over a grain of 900.
    const float FOG_WARP = FOG_GRAIN * 0.5f;

    /// The step between them. Not two, so the tiles never realign and repeat.
    const float FOG_LACUNARITY = 2.27f;

    /// The coarsest level of that chain a march is allowed to read.
    ///
    /// **Not the last one, because a level with too few texels left stops being the field.** Every
    /// level is stretched until a sampler reads one spread out of it, and at two texels a side that
    /// stretch runs the texels into the clamp: the mean drifts and the band clears more than it was
    /// measured against. Measured off the baked volume, the band leaves `FOG_COVERAGE` to within a
    /// twentieth down to four texels a side, then 0.396 at two, and 0.173 at the single texel that
    /// is the whole field's own mean.
    ///
    /// **Four texels is the last level that holds, and that is level three of six.** Past it a step
    /// reads a field it cannot resolve and what comes back is noise, which the jittered step and the
    /// temporal filter take out — and which is what the renderer this is ported from lives with at
    /// every step, having no chain to climb at all.
    const float FOG_FIELD_COARSEST = 3.0f;

    /// What a recorded `Wind Speed` of one comes to in world units a second.
    ///
    /// **Read as a wind rather than picked, which is what it took to make an ash storm look like one.**
    /// The renderer this is ported from first set 120, chosen so the strongest weather crossed one cell
    /// of the coarsest noise in about nine seconds — and nine seconds to cross thirteen metres is 1.4
    /// metres a second, which is a still afternoon rather than a storm. Twenty metres a second is a
    /// Beaufort 8 gale, and seventy units to the metre makes that 1,400. The ten then land where their
    /// names say: clear's 0.1 is a two-metre breeze, rain's 0.3 is six, thunderstorm's 0.5 is ten,
    /// ashstorm's 0.8 is sixteen, and blight and blizzard blow eighteen.
    ///
    /// `mTime` runs at the clock's own rate rather than the game's thirty-times one, so this is a wind
    /// rather than a time-lapse.
    const float FOG_GALE = 1400.0f;

    /// Below `FOG_CLEARING` of the field the air is clear, and at `FOG_SOLID` the fog is at full
    /// thickness. Between them it is a bank's edge.
    ///
    /// **This is what makes fog patchy rather than merely uneven.** Scaling density by a noise gives fog
    /// that is everywhere and varies; cutting a band out of one gives banks with gaps between them,
    /// which is what a valley at dawn looks like.
    ///
    /// **The band has to be cut against the field's own spread, not picked.** Averaging octaves narrows
    /// a distribution sharply, and a threshold chosen for one octave's range clears almost everything:
    /// the renderer this is ported from tried `0.42..1.0` and left average coverage at a third of a per
    /// cent. This field runs mean 0.5 with a standard deviation of `FOG_FIELD_SPREAD` by construction
    /// rather than by measurement, and it does so at every level of the chain — which is what lets one
    /// pair of numbers stand for the band at every step of a march.
    ///
    /// **Sample it over a plane wider than the tile, not over a sphere.** A million pixels of a sphere of
    /// radius 5,000 is a million samples of about a tenth of one tile, and the mean it gives is wrong by
    /// several per cent while looking precise.
    const float FOG_CLEARING = 0.45f;
    const float FOG_SOLID = 0.65f;

    /// What that band comes to on average, which the coverage is divided by.
    ///
    /// **So the noise redistributes the air rather than removing it.** The extinction the host derived
    /// is what a ray should cross on average — it is Morrowind's own view distance, turned into a
    /// coefficient — and a band that clears two thirds of the ground would silently make the world three
    /// times clearer than the game says. Normalised, a bank is 2.9 times the derived extinction against
    /// a gap of nothing, and the average is what it was.
    ///
    /// **Measured, and it must be re-measured if the band or the field moves.**
    /// `theCoverageBandLeavesTheShareTheDensityIsDividedBy` computes it off the baked field to four
    /// figures, and `theBankedFieldHoldsAsMuchAirAsAnEvenOne` checks the frame agrees.
    const float FOG_COVERAGE = 0.3563f;

    /// The mean diameter of the fog's water droplets, in micrometres.
    ///
    /// **The one dial on the shape of the sun's halo.** Radiation fog runs from a few micrometres to
    /// about twenty, and the forward peak sharpens brutally with size: at five the fog scatters 1,300
    /// times an isotropic one straight down the sun's line, at eight 4,300, at thirty 81,000. Eight is
    /// a thick coastal fog.
    const float FOG_DROPLET = 8.0f;

    /// Below this share of what the sky puts into the air, the moons do not get their shadow ray.
    ///
    /// **What makes the cost fall only where the halos are.** Ninety degrees off a moon the phase
    /// function is two thousandths of its forward value, so the moon puts less light into the air
    /// there than the rounding on the sky's term — and a shaft cut out of light that faint is one
    /// nobody can see.
    ///
    /// **The sun's ray is not gated by this**, although the same argument holds for the air: a puff
    /// of smoke in the same froxel reads the sun's shadow whichever way the eye looks, and
    /// `FogSources::mSunward` says so.
    const float FOG_SHAFT_FLOOR = 0.02f;

    /// How many cells of the light grid one ray may walk before it gives up.
    ///
    /// **A budget and not a limit anything reaches.** A froxel's ray is one slice long, and the grid
    /// starts at a cell of one terrain tile, so a ray crosses a handful of cells at most. What this
    /// stops is a fine grid under a long ray turning the walk back into the march it replaced.
    const uint FOG_CELLS_ALONG = 32u;

    /// What is left of a ray at the world's edge, once the second element of the air has had it.
    ///
    /// **The whole point of that element is that this is not a matter of taste.** The last ring of
    /// terrain ends in mid-air, and the only number that hides it is one small enough that the
    /// difference between the ground and the sky behind it is below what the frame can carry. One
    /// step of an eight-bit channel is that number.
    const float FOG_EDGE_TRANSMITTANCE = 1.0f / 256.0f;

    /// Over what share of `VisibilityConstants::mFogEdge` that air closes, as the `1/e` length of
    /// its density.
    ///
    /// **Exponential in the range from the eye, which is what keeps it off the ground the player is
    /// standing on.** A uniform medium thick enough to hide the last cell hazes the first one too. A
    /// density that grows by `e` every eighth of the reach leaves 0.905 of a ray at half of it and
    /// 0.473 at three quarters, so the world closes over its last quarter and the quarter before it
    /// is only softened.
    const float FOG_EDGE_RAMP = 0.125f;

    /// The sine of the climb above which that air is not there at all.
    ///
    /// **A ring on the ground and not a dome, because that is what is missing.** A ray that climbs
    /// leaves the terrain behind and finds sky, which needs no hiding — and air that closed over it
    /// too would put the horizon's colour across the whole upper sky. Twenty-five degrees covers
    /// everything within `tan(25)` of the reach above the eye, which at four cells is fifteen
    /// thousand units of mountain, and leaves the sky over it exactly as it was.
    ///
    /// **A climb alone, and a descent is never masked.** An eye that is high enough looks down on
    /// the ring where the loaded cells stop, so the steeper the view the more of the cut it can see
    /// — and reading this either way would take the air off precisely there.
    const float FOG_EDGE_RISE = 0.4226183f;

    /// Water's index of refraction, and the reflectance it gives head-on.
    ///
    /// `((1.333 - 1) / (1.333 + 1))^2`, which is why water is a window seen from above and a mirror
    /// seen along it.
    const float WATER_IOR = 1.333f;
    const float WATER_F0 = 0.02f;

    /// Extinction per world unit, per channel — how fast water swallows light along a path.
    ///
    /// **Absorption, and not a diffuse attenuation coefficient.** `Kd` is what oceanography usually
    /// quotes and it is the wrong number here twice over: it counts scattering as a loss, and it
    /// counts the lengthening of a path that has been scattered about. This renderer already puts
    /// the scattering back with `WATER_SCATTER`, so charging the beam for it as well is charging it
    /// twice. What is left of a beam is what was absorbed out of it, which is `a`.
    ///
    /// **And scattering takes almost nothing out of a beam here.** The reduced coefficient is
    /// `a + b (1 - g)`, and with `WATER_ASYMMETRY` at 0.92 and the albedo below, `b (1 - g)` comes
    /// to between 0.3 and 1.2 per cent of `a`. Water this forward-scattering loses a beam to
    /// absorption alone, so the term is named rather than carried.
    ///
    /// **Blue last, which is what the albedo beside this one already says.** Pure water absorbs red
    /// twenty-five times as fast as blue, so a body of it reads blue at depth — and a scattering
    /// albedo that peaks in blue is a statement that blue is what survives to be scattered. Written
    /// the other way round, with green surviving longest, the two constants describe two different
    /// waters and the extinction wins: it made every path of any length read green.
    ///
    /// Two terms, per metre, over the visible band weighted by each channel's own response:
    ///
    ///   pure water, Pope and Fry     0.260, 0.054, 0.010
    ///   dissolved organic matter     0.002, 0.005, 0.014     `a(440) = 0.02, exp(-0.014 (l - 440))`
    ///
    /// The second is what makes this a coastal sea rather than an ocean, and it is the one dial:
    /// stained water absorbs blue and nothing else much, so it is what stands between Vvardenfell's
    /// swamp coast and the Pacific. Every expectation a test makes about water derives from this
    /// sum, so a tuning pass is one line rather than five pieces of arithmetic that quietly stop
    /// describing the shader.
    const vec3 WATER_EXTINCTION = vec3(0.262f, 0.059f, 0.024f) / UNITS_PER_METRE;

    /// The single-scattering albedo: the share of extinction that was scattering and not absorption,
    /// and so the part the water hands back as its own colour instead of swallowing.
    ///
    /// **This is what decides whether deep water is dark.** A channel whose scattering albedo
    /// approaches one settles at a bright colour however deep it gets — a milky sheet. Clear
    /// tropical water really does behave that way, because molecular scattering dominates its blue;
    /// a tannin-stained coastal swamp does not, and this game's water is the second.
    ///
    /// **Morrowind's own, which it states as a colour rather than as an albedo.**
    /// `Water_UnderwaterColor` is `012,030,037` and `Water_UnderwaterColorWeight` is 0.85, and
    /// `MWRender::FogManager::getFogColor` mixes them into the weather's fog at exactly that
    /// weight — so `(12, 30, 37) / 255 * 0.85` is the colour the game settles its own murk at.
    /// Read straight across, because the two quantities are the same one: a share of what arrives
    /// that comes back rather than being swallowed.
    ///
    /// **What the game states and this cannot use is the density.** `Water_UnderwaterDayFog` is
    /// 2.5, and `FogManager` runs its ramp from `min(view, 7168) * (1 - depth)` — which for any
    /// depth over two starts *behind* the camera and is 60% complete the moment the eye goes under.
    /// A medium is nought at nought distance by construction, so `Rtx::fogExtinction`'s half-life
    /// match has nothing to bite on: that number is a screen tint rather than a density, and the
    /// absorption above is already the stronger of the two by 7168 units.
    ///
    /// **It peaks in blue, and `WATER_EXTINCTION` is written to agree with it.** A share of what
    /// arrives that comes back is largest where least was taken, so a blue-peaked albedo and a
    /// blue-sparing absorption are one statement about one water. Move either and the other has to
    /// move with it, or the water is two waters again and the extinction is the one that shows.
    ///
    /// **What this asks of the scattering coefficient is not a real water's, and that is the price
    /// of keeping the game's own number.** Read as an albedo it implies `b = a w / (1 - w)`, which
    /// falls toward blue where every real water's rises — molecular scattering goes as the fourth
    /// power of the wavenumber. What that costs is confined to the colour a very deep column
    /// settles at, which is the one thing the game states outright and this defers to.
    const vec3 WATER_SCATTER = vec3(0.04f, 0.1f, 0.1233f);

    /// How far forward water throws what it scatters.
    ///
    /// **Sea water scatters forward far harder than fog does.** Petzold's measurements of the
    /// particle phase function of coastal water give a mean cosine of about 0.92: nearly everything
    /// goes on in the direction it was already travelling, and the sideways part is a thousandth of
    /// the forward peak. That is why an underwater haze is a beam around the sun rather than an even
    /// milkiness, and why looking away from the sun under water is looking into the dark.
    const float WATER_ASYMMETRY = 0.92f;

    /// The waterline, over which water with nothing under it becomes the shore beside it.
    ///
    /// Where the ground rises to meet the surface the depth between them goes to zero, and a pixel of
    /// water with no water in it has to come out as the ground — otherwise the plane cuts the terrain
    /// along a hard line, which is the classic tell of a water plane and is on screen in 533 of the
    /// game's 1,292 land cells. Half a metre is enough to hide the intersection without making the
    /// shallows look thin.
    ///
    /// **Measured straight down, and it was measured along the refraction before.** Those are the same
    /// number only where the bed is flat under the eye. At Seyda Neen's shore the terrain runs within a
    /// few units of sea level for hundreds of units, so the two planes are very nearly parallel — while
    /// the refracted ray, leaving at forty degrees off the vertical, lands far enough out to find a bed
    /// well down. It reported deep water at a pixel with none, the fade never engaged, and what was left
    /// was exactly the hard line this constant exists to prevent. The renderer this is ported from
    /// found the same and says so in its §8.101, and what holds it here is
    /// `theWaterlineIsAsDeepAsTheWaterOverItAndNotAsFarAsARayThroughItGoes`.
    const float WATER_SHORE_FADE = 35.0f;

    /// The scale of the pattern at the focus, in world units, which it grows from.
    ///
    /// **Snyder and Dera's other half, and the one a blur cannot supply.** Their measurement is that the
    /// dominant frequency of the fluctuation falls as the inverse square root of the depth — so the
    /// pattern's own scale grows as the root of it, which is branching and not any kind of blurring.
    /// Both blur terms are linear in the depth and still under one texel at three metres, so left to
    /// them nothing at all changed across the shallows anyone looks at.
    ///
    /// **Fitted against the law rather than derived.** The contrast from two metres to six comes out at
    /// 0.60 of itself here against the 0.58 the root asks for, and a larger grain overshoots it — 0.36
    /// at twelve. It lands within a texel of the wider tile, which is the finest the transform carries
    /// and so the finest a pattern read off it could have had.
    const float WATER_CAUSTIC_GRAIN = 8.0f;

    /// How wide a patch of surface a point one unit down gathers its light from, per unit of depth.
    ///
    /// **Why a caustic coarsens as the water deepens.** Two things blur it and both are angles, so both
    /// grow with the depth: the sun is a disc rather than a point, and the surface presents a spread of
    /// slopes. Together they say a point at depth `d` is lit by a patch this many units across, and
    /// reading the tiles at that footprint is what broadens the pattern as the water deepens. Both are
    /// geometry and both are linear in the depth, which is why they are not the whole of the coarsening:
    /// `WATER_CAUSTIC_GRAIN` carries the part that is not a blur.
    ///
    /// The sun's term is its angular *diameter*, narrowed by refraction on the way in. A mip chain
    /// preserves the mean, so nothing here changes how much light arrives.
    const float WATER_CAUSTIC_SPREAD = 2.0f * SUN_ANGULAR_RADIUS / WATER_IOR;

    /// The depth a sea's caustics are boldest at, in world units.
    ///
    /// **Measured rather than derived, because the sea this renderer synthesises cannot find its own.**
    /// A real ocean's curvature is dominated by waves far shorter than `sShortestWave` — ripples and
    /// capillaries — so its first focus lies within a metre of the surface, which is where Snyder and
    /// Dera found the maximum of the light fluctuation in 1970 and where every field measurement since
    /// has put it. The transform stops at half a metre of wavelength, so left to itself it focuses at
    /// eight, and a bed at six metres came out bolder than one at two. A metre and a half here, which
    /// is the shallow end of what the measurements report.
    ///
    /// **And the carried pattern is normalised to reach its own fold here**, which is the other half of
    /// saying the sea is band-limited. The tiles hold about a fifth of a real sea's curvature, so run at
    /// the literal deflection they would draw a pattern a fifth as bold as the water has — faint at
    /// every depth rather than only at the wrong ones. Scaling instead so the fold lands at the focus
    /// gives the light the strength it is measured to be redistributed with, drawn with the shape the
    /// transform can carry.
    const float WATER_CAUSTIC_FOCUS = 100.0f;

    /// How much of the pattern is drawn, as a share of its own departure from a flat sea.
    ///
    /// **The one number here that answers taste rather than a measurement, and it says so.** Everything
    /// else in this file is the sea differentiated or a figure taken off it; this is how much of the
    /// lens to show. What the arithmetic gives is the whole of it, and the whole of it reads brighter on
    /// a Morrowind shore than the game wants.
    ///
    /// **It scales the departure from one and never the light.** `causticGain` makes the pattern average
    /// to exactly one, and a share of a thing that averages to one still averages to one — so this can be
    /// turned anywhere between nothing and the full lens without the bed receiving a photon more or less
    /// than falls on the water. Multiplying the caustic instead would have taken the light with it.
    ///
    /// The ceiling is not this dial and cannot be. Cutting the cusps lower makes `causticGain` divide by
    /// less, which puts the peak straight back: at a ceiling of 1.4 the brightest place on the bed comes
    /// out where it was, with a gentler shape under it.
    const float WATER_CAUSTIC_STRENGTH = 0.4f;

    /// How fast the pattern fades past the focus, as a power of the depth.
    ///
    /// **A half is what the sea was measured at.** Snyder and Dera's law is that the amplitude of the
    /// fluctuation and its dominant frequency both fall as the inverse square root of the depth, and
    /// that is what a measurement of the ocean says. One is twice that exponent, so the pattern is gone
    /// by twenty metres where the water still has light in it — chosen for the look and not found in
    /// the sea, which is worth saying out loud beside a file full of numbers that were.
    ///
    /// **Blending toward one rather than scaling is what keeps the light wherever this is set**, so the
    /// exponent is free to be turned and the mean does not follow it. Measured at two, six and twenty
    /// metres: 0.213, 0.064 and 0.014 of contrast, where a half leaves the deep end four times bolder.
    const float WATER_CAUSTIC_FADE = 1.0f;

    /// How far toward its own fold the pattern is run at the focus, as a share of the way there.
    ///
    /// **Past one, which is past where a lens has one answer.** At one the determinant first reaches
    /// zero; beyond it the map folds over and a point on the bed is reached by three patches of surface
    /// where this draws one of them. That is what puts the contrast into thin bright filaments, and this
    /// is the dial for how thin they are.
    ///
    /// **Conservation is not what limits it any more.** Run to three, the estimator makes between 13 and
    /// 32 per cent of light depending on how coarsely the cone reads the curvature, and `causticGain` is
    /// the mean of exactly that divided back out. What the fold still costs is coherence: a filament is
    /// the finest thing in the field, so it is made of the fastest-turning waves and it is what moves
    /// first — 67 per cent of the pattern is new a twelfth of a second later, of which the sea carries
    /// 14 points shoreward rather than replacing them.
    const float WATER_CAUSTIC_FOLD = 3.0f;

    /// What share of what a stretch of water sends the sun's own beam has to be before its shaft is
    /// drawn, and where the shaft reaches full strength.
    ///
    /// **A share and not an angle, which is the same test `fogAlong` makes.** An angle sounds like the
    /// right gate — a shaft is the phase function's forward peak — but what decides whether the pattern
    /// can be *seen* is the beam against the sky scattered beside it, and that turns with the hour, the
    /// weather and the depth. Gated at twenty-six degrees the shafts were there only when the sun was
    /// looked straight at; against this they reach as far as they are worth reaching, which at noon in
    /// clear water is past forty-five degrees and at dusk further still.
    ///
    /// **Two of them, because one drew a circle.** A march that begins at a threshold begins with a
    /// pattern already in it, and the ring where that pattern started was the sharpest edge in the
    /// frame. The pattern fades in across the two instead — and the ratio `waterColumn` takes across
    /// them is what makes *nothing to show* come out as exactly the closed form rather than nearly it.
    const float WATER_SHAFT_FLOOR = 0.04f;
    const float WATER_SHAFT_SHOWN = 0.15f;

    /// How many samples a shaft is drawn from.
    ///
    /// The pattern varies along the ray at the scale the surface's own lens does, which is why the steps
    /// are even rather than bunched: unlike the air, there is no density falling off with height for
    /// them to follow, and what wants resolving is spread along the whole stretch.
    const uint WATER_SHAFT_STEPS = 8u;

    /// How far apart the rain's impacts are, in world units: a lattice with one splash a cell.
    ///
    /// **How many rings is not how many drops.** A real rain lands thousands of drops a second on a
    /// square metre and a surface cannot show them as separate rings; what an eye picks out is a few
    /// tens. Twenty units is a dozen impacts on a square metre, with a handful of them ringing at any
    /// moment.
    const float RAIN_RING_CELL = 20.0f;

    /// How long one ring lasts before it has spread into nothing, in seconds.
    const float RAIN_RING_LIFE = 0.6f;

    /// How fast a ring spreads, in world units a second.
    ///
    /// Capillary-gravity waves on water cannot travel slower than 0.23 m/s — where the surface-tension
    /// and the gravity branches of the dispersion relation meet — and a splash ring runs out at about
    /// twice that. Thirty-five units is half a metre a second, so a ring reaches thirty centimetres
    /// before its life is up.
    const float RAIN_RING_SPEED = 35.0f;

    /// The ring's own wavelength, in world units: eleven centimetres, the scale capillary ripples take.
    const float RAIN_RING_LENGTH = 8.0f;

    /// How steep a fresh ring is, as slope at its crest.
    ///
    /// Per ring, and rings overlap — nine cells are summed — so what it comes to as a field is what is
    /// compared against the sea: an rms slope of about a fifth, a third of a running sea's. Enough to
    /// break a reflection where a drop lands, and gone again within the ring's life.
    const float RAIN_RING_STEEPNESS = 0.30f;

    /// The most a texel of a sprite may hide of what is behind it.
    ///
    /// **An alpha of one is an infinite optical depth, and no chord can thin one.** A sprite is
    /// composited as a ball the ray crosses, and what it hides is `1 - (1 - alpha) ^ fraction` for
    /// the fraction of the chord the eye sees — the whole of it in the open, a sliver where the
    /// ball runs into a wall. At an alpha of one that hides everything for any sliver at all, which
    /// is a puff clipped hard at the wall it was meant to fade into. So a texel that says opaque is
    /// taken to mean this, and the price is one part in a hundred of the background through the
    /// densest texel a sprite has.
    const float SPRITE_ALPHA_LIMIT = 0.99f;

    /// What a flame texel of one is worth, as light.
    ///
    /// **A white card square to the sun, which is what the original's one meant.** There a fully
    /// lit surface reached one and an additive sprite at one reached the same white, so the two
    /// stand at one level here: a card under `DAYLIGHT` leaves `DAYLIGHT / pi`. This is not
    /// `EMISSIVE_INTENSITY`, which is a material's convention and not a sprite's, and which sits
    /// about three times this. A flame carried at that scale is more than its own meaning, and at night the
    /// exposure then puts every texel of it past white — the fringe the texture painted goes with
    /// the core, and a sprite reads as a cut-out that switches on.
    const float FLAME_INTENSITY = DAYLIGHT * INV_PI;

    /// How strongly smoke throws the sun forward: Henyey-Greenstein's asymmetry.
    ///
    /// **A puff lit by an even share from every side is a card, and a puff is not a card.** A cloud
    /// of droplets sends most of what it scatters on along the light, so smoke between the eye and
    /// the sun glows and smoke with the sun behind the eye is dim. The even share drew a chimney's
    /// column as bright as the sky from in front and six times darker from behind, which is the
    /// wrong way round. Applied to the sun alone and normalised so the mean over every direction
    /// stays the card's worth; the sky and the lamps arrive from everywhere and keep the even share.
    /// Six tenths is the cloud recipe's figure.
    const float SMOKE_ANISOTROPY = 0.6f;

    /// The most a shell of medium may be thickened by the angle the ray crosses it at.
    ///
    /// **A painted alpha is what one crossing square to the shell hides, and a slanted crossing goes
    /// through more of it.** That is the whole of what a thickness is here: the same slab, `1/cos`
    /// as far through it, so the alpha becomes `1 - (1 - a) ^ (1/cos)`. Head on the content's own
    /// number is kept exactly, which is where a cloud was authored and judged.
    ///
    /// **And it is unbounded at the limb**, where a ray runs along the shell rather than across it —
    /// a secant that goes to infinity draws a hard opaque ring around every cloud in the game. So it
    /// is clamped, and four is where a shell stops thickening: a crossing at fifteen degrees off the
    /// surface, well past where a shell's own curvature has taken over from its slant.
    const float MEDIUM_GRAZE_LIMIT = 4.0f;

    /// The longest history a pixel may keep, in frames.
    ///
    /// **This is the one dial on the trade the accumulator exists to make**, and it is a trade
    /// rather than a setting with a right answer: a longer history is a quieter picture and a later
    /// one. The estimator's error falls as `1/sqrt(n)`, so the return on each further frame is
    /// shrinking while the lag it costs is not — and lag on a bounce shows up as light sliding off
    /// a wall a moment after the lamp that lit it moved.
    ///
    /// **Sixteen is chosen for the lag and not yet measured for the noise**, and saying so is the
    /// point: it is a quarter of a second at sixty frames, which is inside what a player reads as
    /// "the light is on the wall" rather than as a fade. What it is worth against the noise wants a
    /// sweep nobody has run: measured on the grid the filter tests use, sixteen frames take 44% of
    /// the error the spatial cascade cannot reach, but no other count has been tried against it.
    /// Until one is, this is a number picked from the half of the trade that can be reasoned about.
    const float ACCUMULATE_FRAMES = 16.0f;

    /// How far above the running mean a sample may sit before it is taken as an outlier rather than
    /// as light, in standard deviations.
    ///
    /// **A count of sigmas and not a radiance, which is the whole reason this waited for a history.**
    /// An absolute ceiling on the bounce cannot be derived — a lamp's intensity is content, and
    /// `falloff` hands a bounce that lands on one whatever that lamp was given. Against a mean and a
    /// variance the same question has a scene-independent answer: a sample this far from what the
    /// pixel has been seeing is not what the pixel is looking at.
    ///
    /// **Measured, with `shot --tail`**: sixteen accumulated frames take Seyda Neen's tail from 176
    /// pixels over 0.5 to ten, and the clamp takes those ten to three. Where it declines to fire is
    /// an interior full of lamps, because a pixel that sees a bright thing *consistently* raises the
    /// mean to meet it and is never an outlier — which is the design working, not failing.
    ///
    /// Four sigma leaves a Gaussian tail of one sample in sixteen thousand, which at sixteen frames
    /// of history is a clamp that fires on nothing that is really there.
    const float ACCUMULATE_SIGMAS = 4.0f;

    /// How many frames a pixel needs before its second moment describes a spread rather than a
    /// coincidence.
    ///
    /// **Under this the outlier clamp holds off and the cascade is told the pixel is as uncertain as
    /// a pixel can be.** Both are the same admission: a mean of two samples has a variance, and it
    /// is not one anybody should filter by.
    const float ACCUMULATE_SETTLED = 4.0f;

    /// Where the far plane lands once a distance has been scaled for `ACCUMULATE_SURFACE`.
    ///
    /// **A half float is precise in proportion rather than in steps, so what a distance wants from
    /// it is a range and not more bits.** Its normal numbers run from 6.1e-5 to 65504, which is
    /// thirty binades, and a distance from one world unit to a far plane of 200000 needs eighteen of
    /// them. Stored raw the far end overflows at 65504, and every surface past that carries an
    /// infinity `sameSurface` compares against a NaN. Stored as a plain fraction of the far plane
    /// the near end falls to 5e-6, a denormal whose step is 1.2% of the value against a tolerance of
    /// 2%. Putting the far plane at 2^15 does neither: a surface a world unit from the eye stores
    /// 0.164, eleven binades clear of where a half stops holding proportion.
    const float ACCUMULATE_DISTANCE_RANGE = 32768.0f;

    /// How far apart a level's taps stand, doubling each level: 1, 2, 4, 8, 16.
    ///
    /// **Five levels of a 5×5 kernel reach sixty-two pixels.** Each takes two taps at its own
    /// spacing, so the cascade's support is twice `1 + 2 + 4 + 8 + 16`. That is the à-trous trick —
    /// the holes between taps grow while the tap count does not, so a hundred and twenty-five
    /// samples do what a single kernel of that reach would need fifteen thousand for.
    const uint ATROUS_LEVELS = 5;

    /// How much of a froxel's answer comes from where it stood last frame.
    ///
    /// **Heavy, because what it is averaging is one jittered sample.** A froxel takes one point out of
    /// its own volume, one shadow ray for the sun, one for the moons and one for the lamp it held; each
    /// is a draw, and the mean of them is the answer. Nine tenths converges over about ten frames,
    /// which is a sixth of a second — slow enough to hide the draw and quick enough that a shaft
    /// swinging open is not a fade.
    const float FOG_VOLUME_HISTORY = 0.9f;

#ifdef RTX_HOST
}
#endif

#endif
