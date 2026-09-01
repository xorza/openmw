// `#pragma once` everywhere else in this tree, and an include guard here: `glslc` warns
// "'#pragma once' : not implemented" and carries on, so a header included twice by one
// shader would redefine everything in it.
#ifndef OPENMW_COMPONENTS_RTX_SHADERS_SCENE_H
#define OPENMW_COMPONENTS_RTX_SHADERS_SCENE_H

#include "portable.h"

// What the fog volume's two images are made of, said once for both sides that have to agree.
// `gbuffer.h` says why a format is a macro and not a constant, and what it costs when the two
// statements of it drift.
#ifdef RTX_HOST
#define FOG_VOLUME_FORMAT VK_FORMAT_R16G16B16A16_SFLOAT
#else
#define FOG_VOLUME_FORMAT rgba16f
#endif

// The scene's tables, and the scale its brightnesses are measured on, as both sides see them.
// Scalar block layout throughout, so a `uint` is four bytes and a `vec2` is eight on both sides and
// there is nothing to translate.
//
// The constants are here for the same reason the structures are: a number one side derives and the
// other applies has to be one number. Two files each holding their own copy is how a sun and a lamp
// quietly stop being on the same scale.

#ifdef RTX_HOST

#include <cstdint>

#include <osg/Vec2f>
#include <osg/Vec3f>
#include <osg/Vec3ui>
#include <osg/Vec4f>

namespace Rtx::Shaders
{
    using vec2 = osg::Vec2f;
    using vec3 = osg::Vec3f;
    using vec4 = osg::Vec4f;
    using uvec3 = osg::Vec3ui;
    using uint = std::uint32_t;

#endif

    /// A slot of the bindless texture array that is not one.
    ///
    /// **Every slot, and not only a material's.** A cloud deck, a star sheet and a moon's face index
    /// the same array a diffuse map does, so what stands for *nothing loaded* is the same value with
    /// the same meaning — and it used to be that value under three names in two headers, which is a
    /// reader having to check that they agreed.
    RTX_CONST uint NO_TEXTURE = 0xFFFFFFFFu;

    /// Elements in one block of the shared vertex buffers, and of the index buffer.
    ///
    /// **What lets a device buffer be appended to instead of made again.** A buffer that is one
    /// allocation moves when it grows, and every bottom-level acceleration structure in the world
    /// holds a device address into it — so a cell arriving rebuilt all of them. Blocked, the buffer
    /// is a list of allocations made once at full size and never moved: growing costs one more block
    /// and nothing already placed shifts. A shader resolves a global id with `id / BLOCK` and
    /// `id % BLOCK`, which is a shift and a mask because both are powers of two.
    ///
    /// **Bounded below by the largest run one mesh can ask for**, because a run may not straddle a
    /// block. A terrain chunk at full detail is a 65×65 grid and Morrowind's models are far smaller,
    /// so this leaves four orders of magnitude of headroom; what it costs is the tail of a block too
    /// short for the next run, which `Rtx::SpanAllocator` hands out again like any other hole. Three
    /// megabytes of positions a block.
    RTX_CONST uint VERTEX_BLOCK = 256u * 1024u;

    /// The index buffer wants its own number: a triangle soup has three indices a vertex and a
    /// terrain chunk closer to six.
    RTX_CONST uint INDEX_BLOCK = 1024u * 1024u;

    /// Cells along each edge of the grid a texture's baked lighting is estimated over.
    ///
    /// Coarse on purpose: painted lighting varies slowly across a surface and painted detail does
    /// not, so a grid this size follows the first and cannot follow the second. `Rtx::ShadingMap`
    /// makes them and says why at length.
    RTX_CONST uint SHADING_EXTENT = 32u;

    /// A whole turn, which is how a wavelength becomes a wavenumber.
    RTX_CONST float TAU = 6.2831853f;

    /// How many world units the game puts in a metre, which is `Constants::UnitsPerMeter`.
    ///
    /// **Here so that a coefficient measured in a laboratory can stay in the units it was measured
    /// in.** Water's absorption is published per metre and every other number in this file is per
    /// world unit, and a conversion done in a comment is a conversion nothing checks.
    RTX_CONST float UNITS_PER_METRE = 69.99125f;

    /// Morrowind's gravity, in world units per second squared.
    ///
    /// **Multiplied out here rather than written down.** The game states both factors —
    /// `Constants::GravityConst` and `Constants::UnitsPerMeter` — and this is the only place that
    /// wants their product, so writing the product is a third number to keep in step with two.
    RTX_CONST float WATER_GRAVITY = 8.96f * UNITS_PER_METRE;

    /// The circle constant, and the Lambertian BRDF's reciprocal of it.
    ///
    /// Shared because the shader divides every light by `INV_PI` and a lamp's intensity is built
    /// with the matching factor so that the two cancel — a relationship that only holds while both
    /// sides read the same number.
    RTX_CONST float PI = 3.14159265f;
    RTX_CONST float INV_PI = 1.0f / PI;

    /// What an isotropic phase function is worth: one over the solid angle of the whole sphere.
    ///
    /// **A light owes this to the air even with no phase function of its own.** A lamp reaches a
    /// point in the fog as *irradiance*, the same as it reaches a surface, and what comes back
    /// toward the eye is that irradiance spread over every direction — so the air scatters `1/4pi`
    /// of it this way. Left out, lamps light the air twelve and a half times too strongly, which is
    /// a lantern with a white sphere around it rather than a halo.
    RTX_CONST float INV_FOUR_PI = 0.25f * INV_PI;

    /// What the engine paints the second row of its cloud mesh with.
    ///
    /// `ModVertexAlphaVisitor::Clouds`'s own 64 over 255, and the only number in the deck's fade
    /// that is not a radius read off the mesh. `CloudShell::mRings` carries where it applies.
    RTX_CONST float CLOUD_RING_ALPHA = 0.25098f;

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
    RTX_CONST float CLOUD_THICKNESS_MAX = 2.0f;

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
    RTX_CONST float CLOUD_TRANSMISSION = 0.25f;

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
    RTX_CONST float CLOUD_SHADOW_DEPTH = 4.0f;

    /// Irradiance of the sun against the sky it is set in.
    ///
    /// Not a physical figure: exposure absorbs any overall scale, so what matters is the ratio
    /// between the direct sun and the sky, roughly five to one on a clear day on a surface facing
    /// it. Shared with the shader because everything else on this scale is measured against it.
    RTX_CONST float DAYLIGHT = 8.0f;

    /// How many independent numbers one pixel draws in one frame.
    ///
    /// **A channel of the blue-noise tile apiece**, so that two draws a pixel makes are uncorrelated
    /// with each other as well as with its neighbours'. Shared with C++ because the tile is
    /// generated there and has to carry exactly this many masks.
    ///
    /// Exactly the number drawn and not a round one: the fog takes a number, the bounce takes a
    /// pair, and the water's own march takes a number. A spare channel would have to be given a step
    /// to advance by, and the honest step for a stream nobody reads is nothing — which is a value
    /// frozen for the life of the process, waiting for whoever reaches for it next.
    RTX_CONST uint RANDOM_STREAMS = 4;

    /// Which channel of the tile each draw takes. A pair costs two, which is why the bounce leaves
    /// a gap.
    ///
    /// **A channel apiece, not a salt on a shared one.** Every draw a pixel makes has to be
    /// uncorrelated with every other, and the fog's march offset and the bounce's elevation were
    /// literally the same number until the streams were separated — a pixel whose fog started late
    /// also bounced near its normal.
    ///
    /// **Here rather than beside the sampler**, because the count above is a promise these ids have
    /// to keep and the two were a header apart: a second shader that drew would have had to find
    /// this list to know which channels were already spoken for, and nothing pointed at it.
    RTX_CONST uint STREAM_FOG = 0u;
    RTX_CONST uint STREAM_BOUNCE = 1u;

    /// Where the water's shaft march starts inside its first step.
    ///
    /// **Its own channel and not the fog's**, though both are march offsets down one ray: a pixel
    /// whose air started late would have its water start late too, and the two marches lie end to
    /// end along the same line.
    RTX_CONST uint STREAM_WATER = 3u;

    /// Edge of the blue-noise tile, in pixels.
    ///
    /// **Small enough that generating it costs a fraction of a second, large enough that the repeat
    /// does not read as one.** The tile is turned by an irrational step every frame, so what would
    /// be a fixed grid of sixty-four is a different arrangement each time; and the pattern inside it
    /// has no low frequencies to begin with, which is the whole point of it.
    RTX_CONST uint BLUE_NOISE_EXTENT = 64;

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
    RTX_CONST float MOON_RADIANCE = 5.4217f;

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
    RTX_CONST float STAR_RADIANCE = 0.45f;

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
    RTX_CONST float NEBULA_RADIANCE = 0.06f;

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
    RTX_CONST float MOON_ALBEDO = 0.12f;

    /// Angular radius of the sun, in radians — a disc about half a degree across.
    ///
    /// The real figure, because there is only one right answer and nothing about this renderer wants
    /// a different sun. It decides how wide the disc in the sky is drawn, and with it how wide the
    /// glitter path on water is: the two are the same number seen twice, one directly and one in a
    /// mirror, and they cannot be allowed to disagree.
    RTX_CONST float SUN_ANGULAR_RADIUS = 0.004654f;

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
    RTX_CONST float SUN_SHADOW_RADIUS = 0.034907f;

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
    RTX_CONST float MAX_SUN_RADIANCE = 1000.0f;

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
    RTX_CONST float EMISSIVE_INTENSITY = 8.0f;

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
    RTX_CONST float SHEET_TRANSMISSION = 0.5;

    /// How far a ray carries fog before whatever is behind it stops mattering.
    ///
    /// Four hundred metres. Past this the transmittance of even the thinnest weather is a rounding
    /// error, and a ray that hit nothing has to stop somewhere.
    RTX_CONST float FOG_REACH = 30000.0f;

    /// The height over the fog's base at which its density falls to `1/e`, in world units.
    ///
    /// Seventy units to the metre, so about thirty-seven of them — a layer deep enough to fill a
    /// valley and still thin out over the hill beside it.
    RTX_CONST float FOG_HEIGHT = 2600.0f;

    /// How many texels along each side of the fog's baked volume, how many cells of noise it holds
    /// across, and how many levels sit under it.
    ///
    /// **A volume and not a ground plan, because a flat field makes columns.** A field with no third
    /// axis has the same value at every height, so a bank runs from the ground straight up and the
    /// air reads as a stand of pillars rather than as weather.
    ///
    /// **One octave of eight cells, and not a stack of them.** The volume held three octaves over
    /// two cells once, and two cells is eight gradients defining the whole coarse structure — from a
    /// ridge that repeated as a lattice, with its planes drawn as three families of straight lines
    /// across the valley. What makes the fog fractal is the three scales `fogShape` reads it at,
    /// which is what the renderer this is ported from does with a hash and nothing else; the volume
    /// only has to be one octave of noise that does not repeat within a view. Eight cells at four
    /// texels each is the smallest volume that is, and the beat of three scales at `FOG_LACUNARITY`
    /// is what hides the tile past that.
    ///
    /// Two channels and a chain come to 73 kilobytes, which a march reads out of cache.
    RTX_CONST uint FOG_FIELD_SIZE = 32u;
    RTX_CONST uint FOG_FIELD_CELLS = 8u;
    RTX_CONST uint FOG_FIELD_LEVELS = 6u;

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
    RTX_CONST float FOG_FIELD_COARSEST = 3.0f;

    /// The standard deviation every level of that field is normalised to.
    ///
    /// **A property of the field and not of the level a step reached**, which is what lets the
    /// coverage band be one pair of numbers. A level is the mean of the eight texels over it, so its
    /// own spread narrows as the chain goes up, and a band cut against the full level's spread would
    /// clear almost nothing at the top of it.
    ///
    /// The figure is what the field this replaced measured at, so the band and `FOG_COVERAGE` keep
    /// the meanings they were set against.
    RTX_CONST float FOG_FIELD_SPREAD = 0.1204f;

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
    RTX_CONST float FOG_GRAIN = 900.0f;
    RTX_CONST float FOG_TILE = FOG_GRAIN * float(FOG_FIELD_CELLS);

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
    RTX_CONST float FOG_GALE = 1400.0f;

    /// How many scales that one tile is read at.
    ///
    /// **Because one tile repeats and three do not.** A field laid down every twelve thousand units
    /// shows its period across a ray that runs thirty; read again at scales that are not whole fractions
    /// of the first, the three never come back into step, and what is visible is the beat rather than
    /// any one lattice. Three reaches thirty-six units at the fine end, which is finer than any step
    /// a march near the camera takes.
    RTX_CONST uint FOG_SCALES = 3u;

    /// The step between them. Not two, so the tiles never realign and repeat.
    RTX_CONST float FOG_LACUNARITY = 2.27f;

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
    RTX_CONST float FOG_WARP = FOG_GRAIN * 0.5f;

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
    RTX_CONST float FOG_CLEARING = 0.45f;
    RTX_CONST float FOG_SOLID = 0.65f;

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
    RTX_CONST float FOG_COVERAGE = 0.3563f;

    /// What is left of a ray at the world's edge, once the second element of the air has had it.
    ///
    /// **The whole point of that element is that this is not a matter of taste.** The last ring of
    /// terrain ends in mid-air, and the only number that hides it is one small enough that the
    /// difference between the ground and the sky behind it is below what the frame can carry. One
    /// step of an eight-bit channel is that number.
    RTX_CONST float FOG_EDGE_TRANSMITTANCE = 1.0f / 256.0f;

    /// Over what share of `VisibilityConstants::mFogEdge` that air closes, as the `1/e` length of
    /// its density.
    ///
    /// **Exponential in the range from the eye, which is what keeps it off the ground the player is
    /// standing on.** A uniform medium thick enough to hide the last cell hazes the first one too. A
    /// density that grows by `e` every eighth of the reach leaves 0.905 of a ray at half of it and
    /// 0.473 at three quarters, so the world closes over its last quarter and the quarter before it
    /// is only softened.
    RTX_CONST float FOG_EDGE_RAMP = 0.125f;

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
    RTX_CONST float FOG_EDGE_RISE = 0.4226183f;

    /// How many pixels of the frame one column of the fog volume stands for, on each axis.
    ///
    /// **What the volume carries is coarser than a pixel, and both halves of it are.** The field's
    /// finest scale is `FOG_GRAIN` units across, which at any distance worth marching covers far
    /// more than eight pixels; a shaft's edge is a penumbra and not a line. What a smaller number
    /// would buy is a sharper copy of an answer that has no detail at that size, and the volume
    /// costs memory and bandwidth on all three axes at once.
    RTX_CONST uint FOG_VOLUME_SCALE = 8u;

    /// How many slices a column is integrated in.
    ///
    /// **More than the march it replaces takes over one ray**, because a column stands for
    /// `FOG_VOLUME_SCALE` squared pixels and pays once for all of them. The march spends 24 steps
    /// per pixel; this spends 64 per sixty-four pixels.
    RTX_CONST uint FOG_VOLUME_SLICES = 64u;

    /// How many columns one workgroup of the volume pass covers, on each axis.
    RTX_CONST uint FOG_VOLUME_WORKGROUP = 8u;

    /// What shading a hit takes. `Rtx::MaterialKind`, which these must agree with.
    RTX_CONST uint KIND_SURFACE = 0u;
    RTX_CONST uint KIND_TERRAIN = 1u;
    RTX_CONST uint KIND_WATER = 2u;

    /// Water's index of refraction, and the reflectance it gives head-on.
    ///
    /// `((1.333 - 1) / (1.333 + 1))^2`, which is why water is a window seen from above and a mirror
    /// seen along it.
    RTX_CONST float WATER_IOR = 1.333f;
    RTX_CONST float WATER_F0 = 0.02f;

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
    RTX_CONST vec3 WATER_EXTINCTION = vec3(0.262f, 0.059f, 0.024f) / UNITS_PER_METRE;

    /// How far under its nominal level the sea's own surface is placed, in world units.
    ///
    /// **Coplanar surfaces have no intersection order, so one has to be imposed.** Morrowind's
    /// terrain heights are whole multiples of eight units and its sea sits at zero, so ground
    /// authored at sea level is not nearly in the water plane, it is *exactly* in it. A ray then
    /// finds whichever of the two the arithmetic happened to round toward, and that differs from
    /// pixel to pixel: a coastal flat comes back as salt and pepper rather than as either surface.
    /// A rasterizer settles this with draw order and a depth test; a ray tracer has neither.
    ///
    /// **The rasterizer never has to answer this and so never had to decide it.** Upstream draws the
    /// sea as a blended layer with `LEQUAL` and no depth write, over terrain already in the buffer
    /// (`components/sceneutil/waterutil.cpp`), so "which of the two is at this pixel" is not a
    /// question it asks: both are, one over the other, and its own fade with depth makes the layer
    /// contribute nothing where the column is nothing. A ray's first hit is one surface, so the tie
    /// has to be broken rather than blended away.
    ///
    /// It is broken in the ground's favour, which is the reading the content means — a flat the map
    /// puts *at* sea level is a shore and not a lagoon — and it is broken once, in the geometry, so
    /// that it holds for every ray rather than for the one path somebody remembered.
    ///
    /// **The size is bounded at both ends rather than picked.** It has to beat the intersection's
    /// own rounding, which is a few units in the last place of the coordinates: out at the far
    /// corner of the exterior grid those run to some 330,000 units, where a float's last place is
    /// about 0.04, so a handful of them is under a fifth of a unit. And it has to stay far under
    /// anything the eye reads, which the sea itself sets — the waves are metres of amplitude and
    /// this is seven millimetres.
    RTX_CONST float WATER_TIE_BREAK = 0.5f;

    /// Significant wave height over the surface's rms elevation.
    ///
    /// The oceanographers' definition — the mean of the highest third, which for a Gaussian sea is
    /// four standard deviations. It is what `SeaState` normalises its spectrum to, so that the one
    /// figure a person can picture is the one the sea is built from.
    RTX_CONST float WATER_SIGNIFICANT_HEIGHT = 4.0f;

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
    RTX_CONST vec3 WATER_SCATTER = vec3(0.04f, 0.1f, 0.1233f);

    /// How far forward water throws what it scatters.
    ///
    /// **Sea water scatters forward far harder than fog does.** Petzold's measurements of the
    /// particle phase function of coastal water give a mean cosine of about 0.92: nearly everything
    /// goes on in the direction it was already travelling, and the sideways part is a thousandth of
    /// the forward peak. That is why an underwater haze is a beam around the sun rather than an even
    /// milkiness, and why looking away from the sun under water is looking into the dark.
    RTX_CONST float WATER_ASYMMETRY = 0.92f;

    /// A ceiling on how bright a focus is allowed to get.
    ///
    /// Where the refracted bundle collapses to a line the Jacobian goes to zero and the intensity to
    /// infinity — a real caustic *cusp*, and the reason a pool's bright lines are as sharp as they
    /// are. Letting one through would put a pixel in the frame that no exposure could hold.
    ///
    /// **It sets how bright the lines are and not whether there are any**, which is what makes it
    /// the dial to turn. The filaments come from `WATER_CAUSTIC_FOLD` letting the determinant reach
    /// zero; this only says where their tops are cut.
    ///
    /// **Here rather than beside the rest of the caustic's dials, because `causticGain` is fitted
    /// against it.** The clip decides how much of the tail ever arrives, so the two are one
    /// statement and a test that checks the fit has to be able to read both.
    RTX_CONST float WATER_CAUSTIC_MAX = 2.0f;

    /// What the fit below is made of, and the one relation among them that is not fitted: the
    /// numerator's coefficient is the denominator's plus one, which is what makes the curve's second
    /// order exactly `1 + f^2`. Three loose numbers written into the expression would hide it.
    RTX_CONST float WATER_CAUSTIC_GAIN_SQUARE = 1.1017f;
    RTX_CONST float WATER_CAUSTIC_GAIN_CUBE = 0.1872f;
    RTX_CONST float WATER_CAUSTIC_GAIN_QUARTIC = 0.10896f;

    /// The mean of `1 / max(|det(I - b H)|, 1 / WATER_CAUSTIC_MAX)` over a sea of this fold, which
    /// is what the caustic has to be divided by to move light rather than make it.
    ///
    /// **One patch of surface is read for each patch of bed, and that is not how the light is laid
    /// out.** The map from where light met the surface to where it landed is `q = p - b grad(h)`,
    /// and the density at `q` is the reciprocal of its Jacobian. Reading that reciprocal at points
    /// spread evenly over the *surface* rather than weighted by the area each one covers on the
    /// *bed* is a mean of a reciprocal where the reciprocal of a mean was wanted, and it comes out
    /// high. Dividing by that mean is what makes the pattern redistribute the sun exactly.
    ///
    /// **A curve and not a series, which is the whole of why this exists.** The second order of it
    /// is `1 + f^2`, and that is what the shader charged until now — but `WATER_CAUSTIC_FOLD` of
    /// three means `f` has an rms of three, and a second-order expansion in a quantity of order
    /// three describes nothing. It left the bed two metres down 12 per cent dark and twenty metres
    /// down 2 per cent bright, and no coefficient fixed both.
    ///
    /// **It is a hump, and the shape is the ceiling meeting the fold.** Up to about one the Jensen
    /// excess wins and the mean climbs to 1.286; past that the ceiling is cutting cusps faster than
    /// the excess accumulates, so the mean falls back through one at `f = 2.28` and keeps going. A
    /// series can follow the rise and never the fall.
    ///
    /// The fit is to a Monte Carlo over the Hessian of an isotropic Gaussian field — whose entries
    /// have one free parameter, `Var[Hxx] = Var[Hyy] = 3c`, `Var[Hxy] = c`, `Cov[Hxx, Hyy] = c`, so
    /// that `E[(tr H)^2] = 8c` and `f` is the whole of what decides the answer. It agrees with four
    /// million draws to 0.015 at its worst and 0.006 in the mean over folds up to four and a half,
    /// and `RtxCausticGainTest` is what says so. Written so the second order is exact rather than
    /// fitted: the numerator's coefficient is the denominator's plus one.
    ///
    /// @param fold `b` times the root of the curvature variance the cone can still resolve, which is
    ///        how far toward its own first fold the map has been run.
    RTX_SHADER float causticGain(float fold)
    {
        const float squared = fold * fold;

        return (1.0f + (WATER_CAUSTIC_GAIN_SQUARE + 1.0f) * squared)
            / (1.0f + WATER_CAUSTIC_GAIN_SQUARE * squared + WATER_CAUSTIC_GAIN_CUBE * squared * fold
                + WATER_CAUSTIC_GAIN_QUARTIC * squared * squared);
    }

    /// Which instances a ray is interested in.
    ///
    /// **Water must not cast a shadow, and the mask is how traversal is told so at no cost.** The
    /// alternative — building water non-opaque so the candidate loop can wave shadow rays past — was
    /// measured at half the frame rate, because every shadow ray crossing the sea then invokes a
    /// shader where traversal alone had been enough.
    RTX_CONST uint MASK_SOLID = 0x01u;
    RTX_CONST uint MASK_WATER = 0x02u;

    /// The player's own arms in first person: seen by the eye and by no other ray.
    ///
    /// **A pair of hands with no body behind them casts a shadow of a pair of hands**, which the
    /// game never showed — its first-person model wears `Mask_FirstPerson`, and neither of the
    /// shadow-casting masks the rasterizer builds nor the reflection camera's include it. So the
    /// eye's own trace asks for this bit and the shadow rays, the bounces and the water's rays do
    /// not, and the arms are lit and drawn like anything else while shadowing and reflecting as
    /// nothing at all.
    RTX_CONST uint MASK_FIRST_PERSON = 0x04u;

    /// Where a mesh's vertices and indices begin in the shared buffers.
    ///
    /// Indices are mesh-local, so a triangle's vertex is `mVertexOffset` plus what the index says.
    struct GpuMesh
    {
        uint mVertexOffset;
        uint mIndexOffset;

        /// One where the content doubled every triangle of this mesh for its back, which is
        /// `Rtx::MeshRange::mSheet`. With a mask on its material, that is a leaf, and
        /// `SHEET_TRANSMISSION` says what the light on its far side is worth to it.
        uint mSheet;
    };

    struct GpuInstance
    {
        uint mMesh;
        uint mMaterial;

        /// How much of this placement is there, before its material and its texture are asked.
        ///
        /// One for everything the game is not hiding, which is nearly everything. See
        /// `Rtx::MeshInstance::mOpacity` for why a fade belongs to a placement and not to a
        /// material.
        float mOpacity;

        /// World space to where this instance was on the previous frame, as three rows of four.
        ///
        /// **The identity for anything that did not move**, which is nearly everything — and it is
        /// what makes a static surface produce a motion vector of exactly zero rather than one of
        /// rounding. See `Rtx::InstanceRecord::mMotion`.
        vec4 mMotion[3];
    };

    /// One point light, with everything a shader needs already derived.
    ///
    /// The colour is folded into the intensity and the reach is not the radius the record carried;
    /// both are settled on the way in, so the shader has one falloff to evaluate and no rules to
    /// remember. `Rtx::Light` says why each is what it is.
    struct GpuLight
    {
        vec3 mPosition;
        vec3 mIntensity;
        float mReach;

        /// How big the glowing part is, in world units. A shadow ray opens to this, so a lamp with
        /// one casts a penumbra. `Rtx::Light` says what it is derived from.
        ///
        /// **And it is what makes the falloff above a sphere's rather than a point's.** An inverse
        /// square runs away at zero distance, which is where the air beside a lamp is sampled; a
        /// source with an extent flattens inside its own surface instead.
        float mSourceRadius;

        /// How far short of the centre that ray stops. `Rtx::Light` says why it is a separate
        /// question from the size.
        float mClearance;
    };

    /// Where the lamps were binned, so a shader can find the few that reach a point.
    ///
    /// **A property of the scene's lights and not of the camera.** It used to travel in the camera's
    /// push constants, which meant copying it into every frame's block from the buffers it was
    /// derived from — a per-frame copy of something that changes when the cell does.
    ///
    /// A position outside the grid is one no lamp reaches, so its cell is empty by construction
    /// rather than by clamping.
    struct GpuLightGrid
    {
        vec3 mOrigin;
        float mInverseCell;
        uvec3 mSize;
    };

    /// One layer of terrain: a tiling ground texture and the weights that place it.
    ///
    /// A chunk is four or five of these summed. The mask is a grid of weights in the shared mask
    /// buffer rather than a texture, because it is ten texels across — a whole cell's worth fits in
    /// tens of kilobytes, and sampling it by hand is what lets the edges clamp instead of inheriting
    /// the repeat every other texture in the game needs.
    struct GpuLayer
    {
        uint mDiffuse;
        uint mMaskOffset;
        uint mMaskWidth;
        uint mMaskHeight;

        /// Chunk texture coordinates to this layer's, as `uv * xy + zw`.
        vec4 mDiffuseTransform;
        vec4 mMaskTransform;
    };

    /// One live particle, as a disc facing the eye.
    ///
    /// **The layer is composited rather than denoised**, for the reason a rain streak is: an
    /// upscaler carries a transparency layer through its own path, coverage arrives as a fraction so
    /// a sprite finer than a pixel dims instead of flickering in and out, and none of it costs a
    /// bottom-level structure. `Rtx::Sprite` says what each field is.
    struct GpuSprite
    {
        vec3 mPosition;
        float mRadius;
        vec3 mColour;
        float mAlpha;

        /// How far this particle travelled since the last frame, in world units.
        ///
        /// **A displacement and not the position it came from.** The two carry the same fact and not
        /// the same precision: a raindrop's step is a fraction of a unit where its position is six
        /// figures, so subtracting two positions on the device throws away most of the answer before
        /// the reprojection has it. Taken as a difference where both numbers are known exactly and
        /// carried small.
        ///
        /// Zero for a particle born this frame, which is the truth: it has no past to reproject to.
        vec3 mMoved;

        /// Which emitter placed it, which is what a tile's list has to carry.
        ///
        /// **Walking sprites rather than emitters is what made this necessary.** The march evaluates
        /// the fog's field once per emitter per ray — forty hashes, amortised over that emitter's
        /// whole run — and a list of sprites can only keep that amortisation if a sprite can say
        /// when the run it belongs to has changed. It sits in the padding the structure already had.
        uint mEmitter;

        /// How many sprites of its own emitter stand between this one and the sun, and the sky, each
        /// counted for its fade. `Rtx::SpriteShade` counts them once a frame on the host, and
        /// `spritesAlong` thins the light by what one layer of the texture hides.
        float mSunLayers;
        float mSkyLayers;
    };

    /// How many pixels a side one tile of the sprite list covers.
    ///
    /// **Sixteen, and the trade is the usual one.** Finer tiles reject more sprites per pixel and
    /// cost more of them to bin: a raindrop is a few pixels across, so at sixteen it lands in one
    /// tile or four, and a tile's list is short. The screen's tile count is derived from this and the
    /// frame's extent on both sides — `(width + SPRITE_TILE - 1) / SPRITE_TILE` — so there is one
    /// number here and no second one to disagree with it.
    ///
    /// **Eight was measured and it is a loss.** Over Balmora at night in the rain, four times the
    /// tiles take 0.32 ms off the trace and put 2.4 ms on the frame: the offsets are one array entry
    /// per tile, rebuilt and written to the device every frame, and quartering the tile quadruples
    /// that whatever the sprites do.
    RTX_CONST uint SPRITE_TILE = 16u;

    /// The most a texel of a sprite may hide of what is behind it.
    ///
    /// **An alpha of one is an infinite optical depth, and no chord can thin one.** A sprite is
    /// composited as a ball the ray crosses, and what it hides is `1 - (1 - alpha) ^ fraction` for
    /// the fraction of the chord the eye sees — the whole of it in the open, a sliver where the
    /// ball runs into a wall. At an alpha of one that hides everything for any sliver at all, which
    /// is a puff clipped hard at the wall it was meant to fade into. So a texel that says opaque is
    /// taken to mean this, and the price is one part in a hundred of the background through the
    /// densest texel a sprite has.
    RTX_CONST float SPRITE_ALPHA_LIMIT = 0.99;

    /// How much brighter the lit side of a puff is than its mean, and the far side darker.
    ///
    /// **A puff has no dark side and still has a lit one.** A cloud of droplets scatters the sun
    /// through the whole of itself, which is why `puffLight` gives a puff a card's worth of the sun
    /// rather than a sphere's quarter; but the side the sun is on is brighter than the side it is
    /// not, and that is what makes a ball read as a ball. `1 + SPRITE_WRAP * dot(normal, toward)`
    /// keeps the mean over the sphere where it was and puts three to one between front and back.
    RTX_CONST float SPRITE_WRAP = 0.5;

    /// What a flame texel of one is worth, as light.
    ///
    /// **A white card square to the sun, which is what the original's one meant.** There a fully
    /// lit surface reached one and an additive sprite at one reached the same white, so the two
    /// stand at one level here: a card under `DAYLIGHT` leaves `DAYLIGHT / pi`. This is not
    /// `EMISSIVE_INTENSITY`, which is a material's convention and not a sprite's, and which sits
    /// about three times this. A flame carried at that scale is more than its own meaning, and at night the
    /// exposure then puts every texel of it past white — the fringe the texture painted goes with
    /// the core, and a sprite reads as a cut-out that switches on.
    RTX_CONST float FLAME_INTENSITY = DAYLIGHT * INV_PI;

    /// How strongly smoke throws the sun forward: Henyey-Greenstein's asymmetry.
    ///
    /// **A puff lit by an even share from every side is a card, and a puff is not a card.** A cloud
    /// of droplets sends most of what it scatters on along the light, so smoke between the eye and
    /// the sun glows and smoke with the sun behind the eye is dim. The even share drew a chimney's
    /// column as bright as the sky from in front and six times darker from behind, which is the
    /// wrong way round. Applied to the sun alone and normalised so the mean over every direction
    /// stays the card's worth; the sky and the lamps arrive from everywhere and keep the even share.
    /// Six tenths is the cloud recipe's figure.
    RTX_CONST float SMOKE_ANISOTROPY = 0.6;

    /// One particle system: a sphere a ray is rejected by, and the run of sprites behind it.
    struct GpuEmitter
    {
        vec3 mCentre;
        float mReach;
        uint mFirst;
        uint mCount;

        /// The sprite texture. Never `NO_TEXTURE` — an emitter without one places no sprites at all,
        /// since a particle's whole silhouette is that texture's alpha.
        uint mTexture;

        /// Non-zero for `SRC_ALPHA, ONE`. A flame adds and hides nothing; smoke covers and is lit.
        uint mAdditive;

        /// The quad's own axes in world space, per unit of `GpuSprite::mRadius` — **or two zero
        /// vectors, which is a sprite that faces the eye and is nearly everything.**
        ///
        /// `osgParticle` draws a particle as `position ± axisX * size ± axisY * size`. A billboard's
        /// axes are the screen's and need nothing carried here; a `FIXED` system's are used as they
        /// were authored, so its quad hangs in the world at an orientation of its own. Morrowind's
        /// rain is why the mode exists — an X axis squashed to a tenth against a Y axis pointing
        /// straight down is a falling streak rather than a round drop, and their *lengths* are that
        /// shape, so neither is normalised.
        vec3 mAcross;
        vec3 mUpward;

        /// The bake of the sprite texture's alpha, or `NO_TEXTURE`. `Rtx::SpriteLightMap` says what
        /// it holds and `spritesAlong` how it is read.
        uint mLighting;
    };

    struct GpuMaterial

    {
        /// One of the `KIND_` values.
        uint mKind;

        uint mDiffuse;

        /// The alpha below which a texel is a hole, or zero where the surface has none.
        ///
        /// The mode it came from does not survive the trip: what a cutout costs traversal is one
        /// comparison, and a material that wants none stores a threshold nothing can fail. Which
        /// instances stop to make that comparison at all is settled by the build, from the same
        /// number.
        float mAlphaCutoff;

        /// How much of the surface is there, or one for a surface that is all there.
        ///
        /// **The mode does not survive the trip here either.** `Material::isTranslucent` is what
        /// decides, and it settles on the host for the same reason the cutoff does: a leaf card and
        /// a pane of glass carry the same alpha mode, and only the material's own alpha separates
        /// them. A surface that is all there stores a one that nothing has to branch on.
        ///
        /// Multiplied by the texture's alpha at the candidate, which is what a blend does: a stained
        /// pane's texture says where the lead is and this says how much glass there is.
        float mOpacity;

        /// Where this material's terrain layers are, or a count of zero for a single-textured
        /// surface — which is everything but the ground.
        uint mLayerOffset;
        uint mLayerCount;

        /// A map of what glows and how much, or `NO_TEXTURE`. Added past the albedo rather than
        /// through it, which is where the original engine adds it.
        uint mEmissive;

        vec4 mDiffuseColour;

        /// How much the surface glows regardless of what falls on it, with the material's own
        /// multiplier already folded in.
        ///
        /// **A lighting term, not a colour beside one.** The original engine sums it with the
        /// diffuse and ambient light and multiplies the whole by the texture, so a mushroom cap
        /// carrying half against its stalk's nothing glows *with its texture in it*. Added past the
        /// albedo instead, the cap comes out flat white.
        vec3 mEmissiveColour;

        /// Mesh texture coordinates to this material's, as `uv * xy + zw`. The identity for
        /// everything that does not scroll, which is nearly everything.
        vec4 mTextureTransform;
    };

    // **The two C++-shaped sides have to agree byte for byte**, because one writes these buffers and
    // the other reads them — and Metal packs a `float3` differently unless told, which is a mistake
    // that produces a plausible wrong image rather than an error. GLSL is pinned separately, by the
    // `--scalar-block-layout` the build hands the validator.
#if defined(RTX_HOST) || defined(__METAL_VERSION__)
    static_assert(sizeof(GpuMesh) == 12, "GpuMesh must be scalar-packed on every side");
    static_assert(sizeof(GpuInstance) == 60, "GpuInstance must be scalar-packed on every side");
    static_assert(sizeof(GpuLight) == 36, "GpuLight must be scalar-packed on every side");
    static_assert(sizeof(GpuLightGrid) == 28, "GpuLightGrid must be scalar-packed on every side");
    static_assert(sizeof(GpuLayer) == 48, "GpuLayer must be scalar-packed on every side");
    static_assert(sizeof(GpuMaterial) == 72, "GpuMaterial must be scalar-packed on every side");
    static_assert(sizeof(GpuSprite) == 56, "GpuSprite must be scalar-packed on every side");

    static_assert(sizeof(GpuEmitter) == 60, "GpuEmitter must be scalar-packed on every side");

#endif

#ifdef RTX_HOST
}
#endif

// What both shading languages read and nothing on this side calls. The split is about who calls a
// function, not about what a shading language can express: a scalar curve a test has to reach goes
// inside the namespace above, where `RTX_SHADER` makes it `inline` here as well.
#ifndef RTX_HOST

/// Henyey-Greenstein, per steradian: the share of what a medium scatters that leaves `cosine` off
/// the line the light was already travelling.
///
/// **Shared, because the air and the water both want one.** They are the same integral over a
/// different asymmetry — `fogPhase` blends two of these to reach Mie's shape and `WATER_ASYMMETRY`
/// is the water's outright — and two copies of a formula this short are two places for a sign to
/// be wrong.
RTX_SHADER float henyeyGreenstein(float g, float cosine)
{
    const float squared = g * g;
    const float denominator = 1.0 + squared - 2.0 * g * cosine;

    return INV_FOUR_PI * (1.0 - squared) / (denominator * sqrt(denominator));
}

#endif

#endif
