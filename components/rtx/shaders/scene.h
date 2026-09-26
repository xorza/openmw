#ifndef OPENMW_COMPONENTS_RTX_SHADERS_SCENE_H
#define OPENMW_COMPONENTS_RTX_SHADERS_SCENE_H

#include "hosttypes.h"
#include "portable.h"
#include "storageformat.h"

// The scene's tables, and the scale its brightnesses are measured on, as both sides see them.
// Scalar block layout throughout, so a `uint` is four bytes and a `vec2` is eight on both sides and
// there is nothing to translate.
//
// The constants left here are the ones a picture does not turn on: the world's units and the maths
// over them, the sizes a buffer and a grid are built to, the masks traversal reads and the
// sentinels a table spells nothing with. Every number somebody reaches for when the frame looks
// wrong is in `look.h`, which includes this — so a dial and the size it is measured against are
// still one statement, in the direction a light pass can take without taking the tables too.

#ifdef RTX_HOST
namespace Rtx::Shaders
{
#endif

    /// A slot of the bindless texture array that is not one.
    ///
    /// **Every optional map, and never a material's diffuse.** A cloud deck, a star sheet, a moon's
    /// face, a dark, an emissive and an environment map index the same array a diffuse does, so
    /// what stands for *nothing loaded* is one value with one meaning — and a reader of any of
    /// them tests for it before the read, and for `TEXTURE_STANDS_IN` with it (`holdsTexture`).
    /// The diffuse is not optional: a material with none names `TEXTURE_NEUTRAL`, so the albedo,
    /// the opacity, the cutout and the crossing read one path.
    const uint NO_TEXTURE = 0xFFFFFFFFu;

    /// The bit over a slot's texel count (`GpuTables::mTextureTexels`) that says the slot draws the
    /// backend's stand-in and not what was named for it: its file did not read, it is past the side
    /// the device takes, or the device had no room for it.
    ///
    /// **A reader of an optional map reads such a slot as `NO_TEXTURE`.** The stand-in is a grey
    /// picture, and what a missing map means is the reader's to say: a normal map is no relief, a
    /// specular map no lobe and no base colour, a dark map no darkening, a cloud deck no deck — and
    /// each already has its exact path for none. Grey read as any of those is a wrong value and not
    /// a placeholder. Only a base colour draws the grey, where it is one, and the refusal names the
    /// file; a distant chunk's composite that stands in is summed from its stack instead.
    ///
    /// On the device and not in the rows, because it is the backend that decides, as each texture
    /// arrives and as its room comes and goes, and one word per slot follows that where every row
    /// naming the slot would not.
    const uint TEXTURE_STANDS_IN = 0x80000000u;

    /// How many slots the bindless array holds, which is what the backend's descriptor count and
    /// the scene's table are both bounded by.
    const uint TEXTURE_SLOTS = 4096u;

    /// The one slot the scene never hands out: one texel of `NO_TEXTURE_ALBEDO` with an alpha of
    /// one, stood by the backend when the array is made, under the neutral shading map.
    ///
    /// **A texture and not a sentinel, so no reader tests for it.** A material with no diffuse
    /// read `NO_TEXTURE` before every sample it made, and the test was per material, which is per
    /// lane. Naming a real slot instead makes the untextured surface the same path as the textured
    /// one: a one-texel image reads level nought at any cone, and its map is the neutral one every
    /// texture without an estimate already has.
    ///
    /// **The last slot and not the first**, so the scene's own slots stay what they were: its
    /// table hands out from nought, and every test that names a slot by number still does.
    const uint TEXTURE_NEUTRAL = TEXTURE_SLOTS - 1u;

    /// Where the texture set binds its two arrays: the textures, and their shading maps at the
    /// same slots.
    ///
    /// **Named on both sides because a swap would be silent.** Both are `TEXTURE_SLOTS` combined
    /// image samplers, so a layout and a shader that disagreed on which is which would pass every
    /// check the layers make, and the trace would sample shading maps as colour.
    const uint TEXTURE_BIND_IMAGES = 0;
    const uint TEXTURE_BIND_SHADING = 1;

/// What every texture this renderer writes is stored as: a chain a file did not carry, a sprite's
/// light bake and a ground composite. Read back through the file's curve where the file had one.
#define TEXTURE_WRITTEN_FORMAT STORAGE_RGBA8

    /// Elements in one block of the shared vertex buffers, and of the index buffer.
    ///
    /// **What lets a device buffer be appended to instead of made again.** A buffer that is one
    /// allocation moves when it grows, and every bottom-level acceleration structure in the world
    /// holds a device address into it — so a cell arriving would rebuild all of them. Blocked, the
    /// buffer is a list of allocations made once at full size and never moved: growing costs one
    /// more block and nothing already placed shifts. A shader resolves a global id with `id / BLOCK`
    /// and `id % BLOCK`, which is a shift and a mask because both are powers of two.
    ///
    /// **Bounded below by the largest run one mesh can ask for**, because a run may not straddle a
    /// block. A terrain chunk at full detail is a 65×65 grid and Morrowind's models are far smaller,
    /// so this leaves four orders of magnitude of headroom; what it costs is the tail of a block too
    /// short for the next run, which `Rtx::RunAllocator` hands out again like any other hole. Three
    /// megabytes of positions a block.
    const uint VERTEX_BLOCK = 256u * 1024u;

    /// The index buffer wants its own number: a triangle soup has three indices a vertex and a
    /// terrain chunk closer to six.
    const uint INDEX_BLOCK = 1024u * 1024u;

    /// Cells along each edge of the grid a texture's baked lighting is estimated over.
    ///
    /// Coarse on purpose: painted lighting varies slowly across a surface and painted detail does
    /// not, so a grid this size follows the first and cannot follow the second. `Rtx::ShadingMap`
    /// makes them and says why at length.
    const uint SHADING_EXTENT = 32u;

    /// How many world units the game puts in a metre, which is `Constants::UnitsPerMeter`.
    ///
    /// **Here so that a coefficient measured in a laboratory can stay in the units it was measured
    /// in.** Water's absorption is published per metre and every other number in this file is per
    /// world unit, and a conversion done in a comment is a conversion nothing checks.
    const float UNITS_PER_METRE = 69.99125f;

    /// Morrowind's gravity, in world units per second squared.
    ///
    /// **Multiplied out here rather than written down.** The game states both factors —
    /// `Constants::GravityConst` and `Constants::UnitsPerMeter` — and this is the only place that
    /// wants their product, so writing the product is a third number to keep in step with two.
    const float WATER_GRAVITY = 8.96f * UNITS_PER_METRE;

    /// The circle constant, and the Lambertian BRDF's reciprocal of it.
    ///
    /// Shared because the shader divides every light by `INV_PI` and a lamp's intensity is built
    /// with the matching factor so that the two cancel — a relationship that only holds while both
    /// sides read the same number.
    const float PI = 3.14159265f;
    const float INV_PI = 1.0f / PI;

    /// A whole turn, which is how a wavelength becomes a wavenumber. Twice `PI` to the bit, because
    /// doubling a float is exact.
    const float TAU = 2.0f * PI;

    /// What an isotropic phase function is worth: one over the solid angle of the whole sphere.
    ///
    /// **A light owes this to the air even with no phase function of its own.** A lamp reaches a
    /// point in the fog as *irradiance*, the same as it reaches a surface, and what comes back
    /// toward the eye is that irradiance spread over every direction — so the air scatters `1/4pi`
    /// of it this way. Left out, lamps light the air twelve and a half times too strongly, which is
    /// a lantern with a white sphere around it rather than a halo.
    const float INV_FOUR_PI = 0.25f * INV_PI;

    /// How many independent numbers one pixel draws in one frame.
    ///
    /// **A channel of the blue-noise tile apiece**, so that two draws a pixel makes are uncorrelated
    /// with each other as well as with its neighbours'. Shared with C++ because the tile is
    /// generated there and has to carry exactly this many masks.
    ///
    /// Exactly the number drawn and not a round one: the fog's column takes a pair and its march a
    /// number, the bounce takes a pair, and the water's own march takes a number. A spare channel
    /// would have to be given a step to advance by, and the honest step for a stream nobody reads is
    /// nothing — which is a value frozen for the life of the process, waiting for whoever reaches
    /// for it next.
    const uint RANDOM_STREAMS = 6;

    /// Which channel of the tile each draw takes. A pair costs two, which is why the column and the
    /// bounce each leave a gap.
    ///
    /// **A channel apiece, not a salt on a shared one.** Every draw a pixel makes has to be
    /// uncorrelated with every other: a fog offset and a bounce elevation drawn from one number is a
    /// pixel whose fog starts late bouncing near its normal.
    ///
    /// **Here rather than beside the sampler**, because the count above is a promise these ids have
    /// to keep.
    ///
    /// **The column's two offsets are a pair and not two draws of one channel.** Salted apart by the
    /// pixel alone, both turned by the same step each frame, so their difference never changed and
    /// every column walked one diagonal of its block.
    const uint STREAM_FOG_COLUMN = 0u;
    const uint STREAM_BOUNCE = 2u;

    /// Where the water's shaft march starts inside its first step.
    ///
    /// **Its own channel and not the fog's**, though both are march offsets down one ray: a pixel
    /// whose air started late would have its water start late too, and the two marches lie end to
    /// end along the same line.
    const uint STREAM_WATER = 4u;

    /// Where a froxel's sample sits inside its own slice, down the column's ray: a march offset like
    /// the water's, and its own channel for the same reason.
    const uint STREAM_FOG_ALONG = 5u;

    /// What `VisibilityConstants::mNoise` says the per-pixel draws come from: the tile, turned
    /// by an irrational step each frame, or a hashed counter seeded by the pixel, the frame and
    /// the stream. `Rtx::NoiseSource` is the host's spelling and `randomAt` the reader.
    const uint NOISE_BLUE_TILE = 0u;
    const uint NOISE_WHITE_HASH = 1u;

    /// Edge of the blue-noise tile, in pixels.
    ///
    /// **Small enough that generating it costs a fraction of a second, large enough that the repeat
    /// does not read as one.** The tile is turned by an irrational step every frame, so what would
    /// be a fixed grid of sixty-four is a different arrangement each time; and the pattern inside it
    /// has no low frequencies to begin with, which is the whole point of it.
    const uint BLUE_NOISE_EXTENT = 64;

    /// How many texels along each side of the fog's baked volume, how many cells of noise it holds
    /// across, and how many levels sit under it.
    ///
    /// **A volume and not a ground plan, because a flat field makes columns.** A field with no third
    /// axis has the same value at every height, so a bank runs from the ground straight up and the
    /// air reads as a stand of pillars rather than as weather.
    ///
    /// **One octave of eight cells, and not a stack of them.** Three octaves over two cells is eight
    /// gradients defining the whole coarse structure — a ridge that repeats as a lattice, with its
    /// planes drawn as three families of straight lines across the valley. What makes the fog
    /// fractal is the three scales `fogShape` reads it at,
    /// which is what the renderer this is ported from does with a hash and nothing else; the volume
    /// only has to be one octave of noise that does not repeat within a view. Eight cells at four
    /// texels each is the smallest volume that is, and the beat of three scales at `FOG_LACUNARITY`
    /// is what hides the tile past that.
    ///
    /// Two channels and a chain come to 73 kilobytes, which a march reads out of cache.
    const uint FOG_FIELD_SIZE = 32u;
    const uint FOG_FIELD_CELLS = 8u;
    const uint FOG_FIELD_LEVELS = 6u;

    /// The standard deviation every level of that field is normalised to.
    ///
    /// **A property of the field and not of the level a step reached**, which is what lets the
    /// coverage band be one pair of numbers. A level is the mean of the eight texels over it, so its
    /// own spread narrows as the chain goes up, and a band cut against the full level's spread would
    /// clear almost nothing at the top of it.
    ///
    /// The figure is what the band and `FOG_COVERAGE` were set against.
    const float FOG_FIELD_SPREAD = 0.1204f;

    /// How many scales that one tile is read at.
    ///
    /// **Because one tile repeats and three do not.** A field laid down every twelve thousand units
    /// shows its period across a ray that runs thirty; read again at scales that are not whole fractions
    /// of the first, the three never come back into step, and what is visible is the beat rather than
    /// any one lattice. Three reaches thirty-six units at the fine end, which is finer than any step
    /// a march near the camera takes.
    const uint FOG_SCALES = 3u;

    /// How many pixels of the frame one column of the fog volume stands for, on each axis.
    ///
    /// **What the volume carries is coarser than a pixel, and both halves of it are.** The field's
    /// finest scale is `FOG_GRAIN` units across, which at any distance worth marching covers far
    /// more than eight pixels; a shaft's edge is a penumbra and not a line. What a smaller number
    /// would buy is a sharper copy of an answer that has no detail at that size, and the volume
    /// costs memory and bandwidth on all three axes at once.
    const uint FOG_VOLUME_SCALE = 8u;

    /// How many slices a column is integrated in.
    ///
    /// **More than the march it replaces takes over one ray**, because a column stands for
    /// `FOG_VOLUME_SCALE` squared pixels and pays once for all of them. The march spends 24 steps
    /// per pixel; this spends 64 per sixty-four pixels.
    const uint FOG_VOLUME_SLICES = 64u;

    /// How many columns one workgroup of the integrate pass covers, on each axis.
    ///
    /// **A thread to a column there, and that is not a shape to be improved.** Front to back is the
    /// only order transmittance can be carried in, so the sixty-four slices of a column are a scan
    /// and not a fan-out — and the scan is reads and multiply-adds, with no ray and no walk in it.
    const uint FOG_COLUMN_WORKGROUP = 8u;

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
    const float WATER_TIE_BREAK = 0.5f;

    /// Significant wave height over the surface's rms elevation.
    ///
    /// The oceanographers' definition — the mean of the highest third, which for a Gaussian sea is
    /// four standard deviations. It is what `SeaState` normalises its spectrum to, so that the one
    /// figure a person can picture is the one the sea is built from.
    const float WATER_SIGNIFICANT_HEIGHT = 4.0f;

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
    /// **Here rather than in `look.h` with the rest of the caustic's dials, because `causticGain` is
    /// fitted against it.** The clip decides how much of the tail ever arrives, so the two are one
    /// statement and a test that checks the fit has to be able to read both.
    const float WATER_CAUSTIC_MAX = 2.0f;

    /// What the fit below is made of, and the one relation among them that is not fitted: the
    /// numerator's coefficient is the denominator's plus one, which is what makes the curve's second
    /// order exactly `1 + f^2`. Three loose numbers written into the expression would hide it.
    const float WATER_CAUSTIC_GAIN_SQUARE = 1.1017f;
    const float WATER_CAUSTIC_GAIN_CUBE = 0.1872f;
    const float WATER_CAUSTIC_GAIN_QUARTIC = 0.10896f;

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
    /// is `1 + f^2`, but `WATER_CAUSTIC_FOLD` of three means `f` has an rms of three, and a
    /// second-order expansion in a quantity of order three describes nothing: it leaves the bed two
    /// metres down dark and twenty metres down bright, and no coefficient fixes both.
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

    /// Which instances a ray is interested in: the instance mask, one class bit per instance and
    /// `MASK_MEDIUM` beside it, tested against the camera's `VisibilityConstants::mRayMask`.
    ///
    /// **The camera's cull mask, as a ray tracer can read it.** The rasterizer draws what every
    /// node mask on a path intersects the camera's cull mask; the local map's has no actors, no
    /// effects and no particles in it, and the eye's has everything. Here a placement carries the
    /// class the innermost node on its path stated — `Rtx::InstanceClass` — and a trace carries
    /// which classes its camera draws. `MWRender::rayMaskOf` is where the one becomes the other.
    ///
    /// **Water must not cast a shadow, and the mask is how traversal is told so at no cost.** The
    /// alternative — building water non-opaque so the candidate loop can wave shadow rays past —
    /// costs half the frame rate, because every shadow ray crossing the sea then invokes a shader
    /// where traversal alone was enough.
    const uint MASK_STATIC = 0x01u;
    const uint MASK_WATER = 0x02u;

    /// The player's own arms in first person: seen by the eye and by no other ray.
    ///
    /// **A pair of hands with no body behind them casts a shadow of a pair of hands**, which the
    /// game never showed — its first-person model wears `Mask_FirstPerson`, and neither of the
    /// shadow-casting masks the rasterizer builds nor the reflection camera's include it. So the
    /// eye's own trace asks for this bit and the shadow rays, the bounces and the water's rays do
    /// not, and the arms are lit and drawn like anything else while shadowing and reflecting as
    /// nothing at all.
    const uint MASK_FIRST_PERSON = 0x04u;

    /// An actor or the player — `Mask_Actor | Mask_Player` — and an effect the game hung on
    /// something, `Mask_Effect`. **One bit for the actors and the player**, because no camera in the
    /// engine draws one without the other and the eighth bit is the last one.
    const uint MASK_ACTOR = 0x10u;
    const uint MASK_EFFECT = 0x20u;

    /// A camera bit and never an instance's: whether this trace bins and draws the sprites. The
    /// rasterizer's `Mask_ParticleSystem`, which a map tile's camera leaves out.
    const uint MASK_PARTICLE = 0x40u;

    /// A surface that adds to the frame and covers nothing — `Rtx::Material::isAdditive`: a
    /// magic effect's sheet, the Heart of Lorkhan's, the ice wall's. **The last bit, and alone on
    /// its instance.** The eye's trace, the shadow rays, the bounces, the water's rays and the
    /// medium walk never meet one, because none of them casts with this bit; `additiveAlong`
    /// casts with it and nothing else, at the picture's own extent, where the flames are drawn.
    const uint MASK_ADDITIVE = 0x80u;

    /// Every class the eye can ask for. A trace with no camera of its own — the harness's, the
    /// tests' — asks for all of them.
    const uint MASK_EVERY_CLASS
        = MASK_STATIC | MASK_WATER | MASK_FIRST_PERSON | MASK_ACTOR | MASK_EFFECT | MASK_PARTICLE;

    /// What every ray but the eye's own casts with: what the camera draws, less the arms
    /// (`MASK_FIRST_PERSON`), less the water (`MASK_WATER`) which shadows nothing, less the
    /// particles, which are sprites and not instances.
    RTX_SHADER uint solidMask(uint rayMask)
    {
        return rayMask & ~(MASK_FIRST_PERSON | MASK_WATER | MASK_PARTICLE);
    }

    /// What the world's own eye ray casts with: what the camera draws, less the arms, which are
    /// the arms' eye's alone — `VisibilityConstants::mArms`. The rasterizer draws them under a
    /// projection of their own and clears the depth under them, so they stand in front of
    /// everything; here they are traced first, and the world's ray never meets them.
    RTX_SHADER uint worldMask(uint rayMask)
    {
        return rayMask & ~MASK_FIRST_PERSON;
    }

    /// What shelters a falling sprite: the statics and the objects, and nothing that moves. The
    /// rasterizer's `PrecipitationOccluder` draws its depth map with a cull mask of
    /// `Mask_Object | Mask_Static`, so an actor's hat keeps no rain off and a rain-soaked NPC is
    /// the game's own decision rather than this renderer's.
    RTX_SHADER uint shelterMask(uint rayMask)
    {
        return rayMask & MASK_STATIC;
    }

    /// How many see-through surfaces the eye peels off before it draws what is under them.
    ///
    /// **A person is a stack and a window is not.** A pane of glass is one surface; a cuirass over
    /// a skirt over a leg is three, so an actor the game fades — Invisibility, Chameleon, the
    /// distance fade at the edge of `actors processing range` — peeled one layer deep shows its
    /// nearest layer faded and every layer under it at full strength.
    ///
    /// **Four, because that is a dressed person and what is behind them.** The layers are peeled
    /// nearest first and the surface after the last is drawn as the solid it stands in for, so a
    /// deeper stack ends in a surface rather than in a hole. Red Mountain's deepest ray crosses
    /// eight translucent surfaces — counted when the budget was set — and those are the medium's,
    /// which a ray never stops at: `MASK_MEDIUM` says why a shell is gathered rather than met.
    ///
    /// Each layer costs a traversal on the pixels that reach it, and none on a pixel with nothing
    /// see-through in it.
    const uint PEEL_LAYERS = 4u;

    /// How many hit records each closest-hit shader stands behind: one for the eye's own hit and one
    /// for each layer of the peel, so an instance's shader-table offset is its kind times this and a
    /// trace adds the layer it is tracing for. `HitRecord` in `visibility.h` is what a record carries.
    const uint HIT_RECORD_LAYERS = PEEL_LAYERS + 1u;

    /// A surface that is nowhere opaque, gathered as a depth along the ray rather than met.
    ///
    /// **Carried beside the class bit and not instead of it**, because a medium is still something a
    /// shadow ray is dimmed by and something the eye's traversal has to be handed so it can walk
    /// past. What this bit is for is the one ray that wants nothing else: `mediumAlong` traverses on
    /// it alone, so a cell full of shells costs that walk its own instances and no others.
    const uint MASK_MEDIUM = 0x08u;

    /// The material is a medium — `Rtx::Material::isMedium`.
    ///
    /// **A bit and not a second float**, because the row is read at every candidate the eye walks
    /// past. What a medium's density is is not stored: the texture's own alpha is what a crossing
    /// square to the surface hides, and the obliquity is the whole of what a thickness adds to it.
    const uint MATERIAL_MEDIUM = 0x01u;

    /// The mesh's per-vertex colour replaces this material's diffuse tint —
    /// `Rtx::VertexColour::Tint`, which is every piece of ground and over half of the models
    /// the game ships.
    ///
    /// **A bit and not a second colour on the row.** The two are exclusive, a mesh that brought no
    /// colour holds white, and what the shader does with either is one `mix` against a weight of
    /// nought or one — so a surface that carries neither pays no branch and no extra load.
    const uint MATERIAL_VERTEX_TINT = 0x02u;

    /// The same colour replaces this material's glow instead — `Rtx::VertexColour::Glow`. The
    /// light mode that goes with it already took the diffuse and the ambient to nought, so such a
    /// surface is its glow and nothing else.
    const uint MATERIAL_VERTEX_GLOW = 0x04u;

    /// The surface adds whole, `ONE, ONE`: its alpha unread. That it adds at all is said by the
    /// mask its placements carry, `MASK_ADDITIVE` alone, which is what brings it to
    /// `additiveAlong` and to nothing that shades a hit — so no bit here says so twice.
    const uint MATERIAL_ADD_WHOLE = 0x08u;

    /// Ground that kept its layer stack: the albedo is the sum over `mLayerOffset`'s run and the
    /// diffuse is the neutral slot. A chunk far enough to be flattened names its composite as the
    /// diffuse instead and carries this bit no longer — `CellPlacer::wantsFlattening` is where the
    /// two swap, and this is the host's one rule for which a row is, written where the row is.
    const uint MATERIAL_STACKED = 0x10u;

    /// The normal map's alpha is a height the texture coordinates are shifted by —
    /// `Rtx::Material::mParallax`, and `parallaxShift` says by how much.
    const uint MATERIAL_PARALLAX = 0x20u;

    /// Which texture unit the dark map is bound at, in these bits of `mFlags` —
    /// `GpuMesh::mUnitStreams` says which stream that unit reads.
    const uint MATERIAL_DARK_UNIT_SHIFT = 8u;
    const uint MATERIAL_DARK_UNIT_MASK = 0x0Fu;

    /// A sprite emitter that adds — `Rtx::BlendKind::Add`, or `AddWhole` with its sprites' alpha
    /// settled at one by the resolver — and one whose sprites fall from the sky:
    /// `spriteshelter.rgen` drops those that stand under cover.
    const uint EMITTER_ADDITIVE = 0x01u;
    const uint EMITTER_FALLS = 0x02u;

    /// The content doubled every triangle of this mesh for its back — `Rtx::FoldedShape::mSheet`.
    /// With a mask on its material that is a leaf, and `SHEET_TRANSMISSION` says what the light on
    /// its far side is worth to it.
    const uint MESH_SHEET = 0x01u;

    /// Every edge of this mesh carries a triangle each way — `Rtx::FoldedShape::mClosed`. It says
    /// which of a surface's two normals describes it, which `litCosine` reads.
    const uint MESH_CLOSED = 0x02u;

    /// The mesh carries tangents — `Rtx::MeshRange::mTangents` — so a hit on it has a frame to read
    /// a normal map through. **What a traversal asks before it fetches three tangent words**, on the
    /// mesh row it already holds: a vanilla scene has none, and every hit in it skips the fetch.
    const uint MESH_TANGENTS = 0x04u;

    /// Where a mesh's vertices and indices begin in the shared buffers.
    ///
    /// Indices are mesh-local, so a triangle's vertex is `mVertexOffset` plus what the index says.
    struct GpuMesh
    {
        uint mVertexOffset;
        uint mIndexOffset;

        /// What the fold found this mesh's triangles to be — `MESH_SHEET` and `MESH_CLOSED`.
        ///
        /// **Bits and not two words, because this row is read on every hit.** A mesh table entry is
        /// six words and every ray that lands fetches one.
        uint mShape;

        /// Where this mesh's second set of texture coordinates begins in the blocks of their own,
        /// or `NO_STREAM` for a mesh that brought none, which is nearly every mesh. Mesh-local, as
        /// `mVertexOffset` is: the vertex's index within the mesh is added to it.
        uint mSecondTexCoordOffset;

        /// One bit per texture unit, set where that unit reads the second set —
        /// `Rtx::MeshArrays::mUnitStreams`. A material names the unit its dark map is bound at and
        /// this says which stream that unit reads, because the material is shared across geometries
        /// and the binding is each geometry's own.
        uint mUnitStreams;

        /// Where this mesh's posed vertices sit among the deforming meshes' — `Rtx::MeshRange::
        /// mBindOffset`, the index the pose blocks are addressed by — or `NO_STREAM` for a mesh
        /// that stands. What lets a hit on a body read where its triangle stood last frame: the
        /// one field a moving surface's motion cannot do without, and the sixth word of the row.
        uint mBindOffset;
    };

    /// A mesh with no second set of texture coordinates.
    const uint NO_STREAM = 0xFFFFFFFFu;

    /// A vertex's tangent as one word: the direction folded onto the octahedron, each of its two
    /// coordinates stepped to `2 * TANGENT_STEPS + 1` values in fifteen bits, the handedness of the
    /// bitangent, and whether the vertex has a tangent at all. Nought is none, which is every vertex
    /// of a mesh no normal map is read through. `Rtx::packTangent` writes it and `unpackTangent`
    /// reads it on either side.
    ///
    /// **An odd count of steps**, so that nought, one and minus one are exact: an axis-aligned
    /// tangent, which a quad mapped square to its texture has, comes back as the axis it was.
    const uint TANGENT_PRESENT = 0x80000000u;
    const uint TANGENT_FLIPPED = 0x40000000u;
    const uint TANGENT_COORDINATE_BITS = 15u;
    const uint TANGENT_COORDINATE_MASK = (1u << TANGENT_COORDINATE_BITS) - 1u;
    const uint TANGENT_STEPS = TANGENT_COORDINATE_MASK / 2u;

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

    /// One light placed in the world — a lamp, or the fill a magic effect glows with — with
    /// everything a shader needs already derived: a `LIGH` record carries a colour and a radius and
    /// no intensity at all, and `Rtx::makeLight` settles both on the way in, so the shader has one
    /// falloff to evaluate and no rules to remember.
    ///
    /// **The scene's own row, and the device's.** `Rtx::Light` is this struct: the walk builds
    /// one, the scene keeps and sorts them, the digest hashes the table whole, and a placement
    /// uploads the table as it lies. There is no host spelling to translate from.
    struct GpuLight
    {
        vec3 mPosition;

        /// Radiant intensity, linear, with the colour folded in, scaled by the square of the
        /// recorded radius: what makes a lantern and a candle differ by their size.
        vec3 mIntensity;

        /// How far the light reaches, beyond which it contributes exactly nothing. Stretched from
        /// the recorded radius, because Morrowind's ran 64 to 256 units with an ambient filling the
        /// room, and here the lamps have to be what lights the place.
        float mReach;

        /// How big the glowing part is, in world units: the flame, which a shadow ray opens to for
        /// a penumbra as wide as it is, and what stops the falloff running away at the lamp. Zero,
        /// which a light built by hand carries, is a point.
        ///
        /// **And it is what makes the falloff above a sphere's rather than a point's.** An inverse
        /// square runs away at zero distance, which is where the air beside a lamp is sampled; a
        /// source with an extent flattens inside its own surface instead.
        float mSourceRadius;

        /// How far short of the centre a shadow ray stops, because a lamp sits inside its own
        /// fitting and a ray that runs all the way ends among it.
        float mClearance;

        /// One where this light is a fill and nought where it is a lamp. A fill is a lamp whose
        /// flame is a ball `mSourceRadius` wide, lit from every side inside it: a magic effect's
        /// glow, which `Rtx::Glow::makeLight` builds. A word and not a bool, because the record is
        /// hashed whole and a bool leaves three bytes nothing wrote.
        uint mFill;
    };

    /// Where the lamps were binned, so a shader can find the few that reach a point.
    ///
    /// **Carried in the frame's block, as `VisibilityConstants::mLightGrid`**, which is a uniform
    /// buffer written once a frame that already folds in the sea's tables from the passes that built
    /// them. A twenty-eight byte record in a set of its own would cost a descriptor, a buffer per
    /// frame in flight, and a storage read at every lamp lookup where the constant bank serves.
    ///
    /// A position outside the grid is one no lamp reaches, so its cell is empty by construction
    /// rather than by clamping.
    struct GpuLightGrid
    {
        vec3 mOrigin;
        float mInverseCell;
        uvec3 mSize;
    };

    /// Where every table a hit reads is, as one address apiece.
    ///
    /// **In the frame block rather than in a descriptor each**, for the reason the light grid's
    /// geometry already is: a descriptor per table is seventeen storage-buffer bindings pushed twice
    /// a frame, and a binding the layout declares and the pass forgets is a shader reading whatever
    /// the slot holds. An address is a 64-bit integer, so the struct belongs to the scene and not to
    /// a backend: the Vulkan shader constructs a `buffer_reference` from each.
    ///
    /// **Filled by the pass and not by `describeWorld`**, the way `mWaveExtent` and `mLightGrid`
    /// are: where a table is lives with whatever placed it there, and the tables that alternate by
    /// frame slot change address every frame.
    ///
    /// **No size beside an address.** A descriptor carries one and robust access bounds a read by
    /// it; a pointer carries none. What stops a shader reading past a table is its count, and what
    /// reports one that does is GPU-assisted validation's address table.
    ///
    /// **What it costs is a load path and not an instruction count** — a few per cent of an
    /// interior's trace and nothing on the exteriors — accepted as the price of six bindings and of
    /// a class of mistake gone.
    struct GpuTables
    {
        /// The five tables of block addresses, which a global vertex or index id is resolved
        /// through. The normals and the tangents are this slot's copy.
        uint64 mNormalBlocks;
        uint64 mTangentBlocks;
        uint64 mTexCoordBlocks;
        uint64 mColourBlocks;
        uint64 mIndexBlocks;

        /// The second texture coordinates, in blocks of their own that only the meshes carrying a
        /// second set take from — `GpuMesh::mSecondTexCoordOffset` is where a mesh's run begins.
        uint64 mSecondTexCoordBlocks;

        uint64 mMeshes;
        uint64 mInstances;
        uint64 mMaterials;
        uint64 mLayers;
        uint64 mMasks;
        uint64 mLights;

        /// The light grid's list: where each cell's run starts, then the runs. `Rtx::LightGrid`
        /// says why the starts and the runs are one list.
        uint64 mLightList;

        uint64 mBlueNoise;

        /// The GGX lobe's two integrals over the cosine to the eye and the roughness,
        /// `Rtx::SpecularAlbedo`: made once and read for every glossy surface's compensation and
        /// its specular albedo.
        uint64 mSpecularAlbedo;

        uint64 mSprites;
        uint64 mEmitters;

        /// One `GpuEmitterFrame` a row of `mEmitters`, the trace's own like the sprites.
        uint64 mEmitterFrames;

        /// The sprite tiles' list, in the same shape over the screen's tiles.
        uint64 mSpriteTileList;

        /// One word a screen tile of the same grid: the `PRESENCE_` kinds of instance a ray through
        /// the tile can meet, which `SpriteBin` ORs in from `GpuPresence`'s spheres.
        uint64 mSpritePresence;

        /// The pose blocks, this copy's and the other's: every deforming mesh's vertices as this
        /// frame traces them and as the previous frame did, by `GpuMesh::mBindOffset` plus the
        /// vertex's index within the mesh. The copies are owed every write and paid at every
        /// sync, so the copy this frame does not trace holds the pose as of the frame before —
        /// and for a mesh that did not move, the same numbers as this one's, so its delta is
        /// exactly nought. `reproject.glsl` reads the pair, in object space, where a difference
        /// of two positions is exact.
        uint64 mPoseBlocks;
        uint64 mPreviousPoseBlocks;

        /// One `uint` per slot of the bindless array: how many texels its texture holds, which is
        /// the one term of a mip level that is the texture's own — `coneLod`. A load where a
        /// `textureSize` was a texture-header read on every sample, and stated over the same
        /// integer so the level the shader takes its logarithm of is the number it always was.
        /// The backend's texture array owns and writes it, a slot at a time as textures arrive, with
        /// `TEXTURE_STANDS_IN` over the count of a slot that draws the stand-in.
        uint64 mTextureTexels;
    };

    /// What a reference to each table may claim about its address, and so what the host checks.
    ///
    /// **The largest power of two that divides both the buffer's start and every element access.**
    /// A claim larger than the truth is undefined behaviour with no message. A claim smaller than
    /// the truth costs the compiler a wider load where one was possible. A buffer's start is at
    /// least sixteen-aligned on this device and the host asserts it, so the stride decides:
    /// `GpuLayer` is 64 bytes with two `vec4` at sixteen and thirty-two, the block tables hold
    /// eight-byte addresses, and every other row or list is four-aligned only.
    const uint TABLE_ALIGN_ROWS = 4u;
    const uint TABLE_ALIGN_BLOCKS = 8u;
    const uint TABLE_ALIGN_LAYERS = 16u;

    /// A ground layer whose diffuse is an authored albedo with the perceptual roughness in its
    /// alpha — a `_diffusespec` under `SpecularLayout::MetalRoughness`, as Wareya's shaders read it.
    /// **Not delit, and a dielectric**: the texture was painted as a surface and not as a picture
    /// of one lit, and ground is no metal, so its reflectance at normal incidence is
    /// `DIELECTRIC_F0`. Under the classic layout the same file is the plain diffuse OpenMW swaps
    /// in, and this is not set.
    const uint LAYER_AUTHORED = 0x01u;

    /// A ground layer whose normal map's alpha is a height, as `MATERIAL_PARALLAX` is for a
    /// surface: an `_nh` file the storage found, which `carriesHeight`. The rasterizer's terrain
    /// shifts the layer by it, and so does the stack here; a flattened chunk has no eye to shift
    /// toward.
    const uint LAYER_PARALLAX = 0x02u;

    /// One layer of a terrain material: a tiling ground texture and the weights that place it.
    ///
    /// A chunk is four or five of these summed at one hit, where OpenMW draws the stack as one
    /// alpha-blended pass per layer. The mask is a grid of weights in the shared mask buffer
    /// rather than a texture, because it is ten texels across — a whole cell's worth fits in tens
    /// of kilobytes, and sampling it by hand is what lets the edges clamp instead of inheriting the
    /// repeat every other texture in the game needs.
    ///
    /// **The scene's own row, and the device's.** `Rtx::MaterialLayer` is this struct, carried
    /// whole from the land record through `Rtx::PreparedLayer` to the scene's layer run and the
    /// bake, so the three cannot disagree about where a texel lands. The grid and the two
    /// transforms come from the land record; the texture slot and the mask offset are the
    /// scene's, written where the layer is adopted.
    struct GpuLayer
    {
        /// The ground texture, which tiles many times across a chunk.
        uint mDiffuse;

        /// Where this layer's weights start in the scene's mask table. The run's count is not
        /// stored: it is the grid's own area, and nought for a layer that covers everything —
        /// `Rtx::maskOf` reads the run back.
        uint mMaskOffset;

        /// The grid the weights form. Nought by nought where the layer covers everything.
        uint mMaskWidth;
        uint mMaskHeight;

        /// Cell texture coordinates to this layer's, as `uv * xy + zw`: the diffuse texture's, and
        /// the mask's. `Rtx::GroundReader` derives both from the tile count as
        /// `Terrain::createPasses` does, and a test holds the numbers.
        vec4 mDiffuseTransform;
        vec4 mMaskTransform;

        /// The layer's normal map, read at the diffuse's coordinates, or `NO_TEXTURE`. Its
        /// tangent is the one `terrain.vert` gives every layer: the chunk's x, which is the
        /// world's, with the bitangent `cross(N, x)`.
        uint mNormal;

        /// `LAYER_` bits.
        uint mFlags;

        /// To the sixty-four bytes std430 would give the row: at fifty-six, every other row's two
        /// `vec4` sit on eight and not sixteen, and every layer a hit sums is read in eight-byte
        /// loads, vanilla ground included.
        uint mPadding[2];

#ifdef RTX_HOST
        /// Two layers are the same when every field is, which is what says a chunk still stands
        /// where a bake of it began.
        bool operator==(const GpuLayer& other) const = default;
#endif
    };

    /// One live particle, drawn as a disc facing the eye. Nothing here reaches an acceleration
    /// structure: the layer is marched against the primary ray and composited, which blends in
    /// depth order without the candidate loop an alpha-blended hit would cost traversal.
    ///
    /// **The layer is composited rather than denoised**, for the reason a rain streak is: a
    /// particle is not noise in an estimate, coverage arrives as a fraction so a sprite finer than a
    /// pixel dims instead of flickering in and out, and none of it costs a bottom-level structure.
    /// `spritecomposite.rgen` composites it at the picture's own resolution, after whatever
    /// denoises.
    ///
    /// **The scene's own row, and the device's.** `Rtx::Sprite` is this struct: the resolver
    /// reads one off each live particle, `Rtx::SceneDesc::addEmitter` names its emitter as it
    /// appends it, the digest hashes the table whole, and a placement uploads it as it lies. The
    /// two layer counts at the end are the device's alone, and the host leaves them at nought.
    struct GpuSprite
    {
        vec3 mPosition;

        /// Half the sprite's width in world units, which is what `osgParticle` means by a size.
        float mRadius;

        /// The streak's own axis in the world, per unit of `mRadius`, or zero for a sprite that
        /// faces the eye. Per particle, because `Weather::RainShooter` leans each drop into the wind
        /// it was fired under. Not normalised, because its length is the shape.
        vec3 mAxis;

        /// Linear, and already carrying wherever the particle's own colour ramp has reached.
        vec3 mColour;

        /// What the particle's own fade left of it, multiplied into the texture's alpha at the hit.
        float mAlpha;

        /// Which emitter placed it, which is what a tile's list has to carry. Written by
        /// `Rtx::SceneDesc::addEmitter`, the one place that knows.
        ///
        /// **Walking sprites rather than emitters is what made this necessary.** The march evaluates
        /// the fog's field once per emitter per ray — forty hashes, amortised over that emitter's
        /// whole run — and a list of sprites can only keep that amortisation if a sprite can say
        /// when the run it belongs to has changed.
        uint mEmitter;

        /// How many sprites of its own emitter stand between this one and the sun, and the sky, each
        /// counted for its fade. `spriteshade.h` counts them once a frame on the device, and
        /// `spritesAlong` thins the light by what one layer of the texture hides.
        float mSunLayers;
        float mSkyLayers;
    };

    /// How many pixels a side one tile of the sprite list covers.
    ///
    /// **Sixteen, and the trade is the usual one.** Finer tiles reject more sprites per pixel and
    /// cost more of them to bin: a raindrop is a few pixels across, so at sixteen it lands in one
    /// tile or four, and a tile's list is short. The screen's tile count is derived from this and the
    /// frame's extent on both sides — `spriteTilesOver` — so there is one number here and no second
    /// one to disagree with it.
    ///
    /// **Eight is a loss.** A drop's own test is cheap once the lamps are walked per emitter, so
    /// the trace gains nothing from four times the tiles, while the fill, which walks every sprite
    /// for every tile, pays for all of them.
    const uint SPRITE_TILE = 16u;

    /// How many tiles cover `pixels` along one axis of the frame. The last one may be part of a tile.
    ///
    /// **Derived on every side from `SPRITE_TILE` and the frame's own extent**, so the trace, the
    /// bin and the host reference cannot disagree about how many tiles there are across.
    RTX_SHADER uint spriteTilesOver(uint pixels)
    {
        return (pixels + SPRITE_TILE - 1u) / SPRITE_TILE;
    }

    /// How many tiles a frame of `width` by `height` covers, which is the list's head less one.
    ///
    /// **The product, in one place, because four sides take it.** The scan, the fill, the pass's own
    /// zeroing fill and the host's sizing each need how many tiles a frame has, and each multiplied
    /// the two axes for itself — four chances for the head to be one length here and another there,
    /// over a list every one of them then indexes.
    RTX_SHADER uint spriteTilesIn(uint width, uint height)
    {
        return spriteTilesOver(width) * spriteTilesOver(height);
    }

    /// What the sprite tiles' list holds in its first entry where its runs did not fit.
    ///
    /// **The list carries its own degenerate form, so the trace needs no second signal.** Where
    /// the runs are binned, entry nought is where the runs begin — `tiles + 1`, never nought. Where
    /// a frame's entries outgrew the buffer, `spritestarts.comp` writes nought there and the sprite
    /// count in entry one, and the trace walks every sprite over every pixel for that frame: slow
    /// and right. The host reads what the frame needed,
    /// grows the buffer and the next frame is binned. `SpriteBin::record` says how the list
    /// is sized so that this is a rare frame and never a wrong one.
    const uint SPRITE_LIST_UNBINNED = 0u;

    /// How much brighter the lit side of a puff is than its mean, and the far side darker.
    ///
    /// **A puff has no dark side and still has a lit one.** A cloud of droplets scatters the sun
    /// through the whole of itself, which is why `puffLight` gives a puff a card's worth of the sun
    /// rather than a sphere's quarter; but the side the sun is on is brighter than the side it is
    /// not, and that is what makes a ball read as a ball. `1 + SPRITE_WRAP * dot(normal, toward)`
    /// keeps the mean over the sphere where it was and puts three to one between front and back.
    const float SPRITE_WRAP = 0.5;

    /// One particle system: what its sprites are drawn with, and a sphere that holds all of them,
    /// which is the whole spatial structure because one rejection throws a small emitter away for
    /// almost every pixel.
    ///
    /// **The scene's own row, and the device's.** `Rtx::SpriteEmitter` is this struct, built by
    /// `Rtx::SceneDesc::addEmitter` and uploaded as it lies. The flags are read through the two
    /// members below, and the run through `Rtx::spritesOf`, which needs the host's `Run`.
    struct GpuEmitter
    {
        vec3 mCentre;

        /// Far enough from `mCentre` to contain every sprite in the run, rim included.
        float mReach;

        /// Where the sprites sit in the scene's sprite table, laid end to end as the emitter
        /// placed them — `Rtx::spritesOf` reads the run back.
        uint mFirst;
        uint mCount;

        /// The sprite texture. Never `NO_TEXTURE` — an emitter without one places no sprites at all,
        /// since a particle's whole silhouette is that texture's alpha.
        uint mTexture;

        /// `EMITTER_ADDITIVE` for a blend that adds — `SRC_ALPHA, ONE`: a flame, which adds light
        /// and hides nothing behind it, where the rest blend over and are smoke that needs its
        /// colour ramp to fade it — and `EMITTER_FALLS` for what the weather drops, the rain box
        /// or a driven storm, which `spriteshelter.rgen` keeps out from under a roof.
        /// `Rtx::SceneExtractor::extractFalling` is the walk that says so.
        uint mFlags;

        /// How wide this emitter's quads are against their own axis, per unit of
        /// `GpuSprite::mRadius` — **or nought, which is a sprite that faces the eye and is nearly
        /// everything.** Morrowind's rain is an X axis squashed to a tenth against a Y axis
        /// pointing straight down. The length and not the direction, because the march swings the
        /// width about the sprite's own axis to meet the ray.
        float mWidth;

        /// The bake of the sprite texture's alpha, or `NO_TEXTURE` for one lit as a flat card.
        /// `Rtx::SpriteLightMap` says what it holds and `spritesAlong` how it is read.
        uint mLighting;

#ifdef RTX_HOST
        bool isAdditive() const
        {
            return (mFlags & EMITTER_ADDITIVE) != 0u;
        }
        bool falls() const
        {
            return (mFlags & EMITTER_FALLS) != 0u;
        }
#endif
    };

    /// What one trace makes of an emitter, once for every sprite of it: `spriteemitters.rgen` writes
    /// a row an emitter ahead of the trace, and both walks of `spritesAlong` read it where they
    /// meet the emitter's run. Each is the emitter's and the camera's alone, where a walk worked it
    /// out again at every pixel the emitter covers — and at the shown extent a second time.
    struct GpuEmitterFrame
    {
        /// The fog's coverage band over the path from the eye to the emitter, taken at the path's
        /// mean-value point: every sprite of the emitter is within `GpuEmitter::mReach` of the same
        /// air, and the band costs forty hashes.
        float mBand;

        /// What one layer of the emitter's texture lets through on average, as the base-two
        /// logarithm the walk's two powers share: off its coarsest level, held under
        /// `SPRITE_ALPHA_LIMIT`.
        float mLayerThrough;

        /// The texture's extent along each of its axes.
        vec2 mTexels;
    };

    /// The kinds of instance the walks along a primary ray look for, and what a tile of
    /// `GpuTables::mSpritePresence` holds: a surface that adds to the frame — a magic effect's
    /// sheet, `additiveAlong` — and one the eye passes through, a cloud's shells, `mediumAlong`.
    /// **What keeps each walk off the pixels it cannot find anything at.** Both traverse the top
    /// level on a mask a handful of instances carry, once a pixel, and asked of a whole frame one
    /// such mesh anywhere in the loaded cells had every pixel descend it to find nothing.
    const uint PRESENCE_ADDITIVE = 1u;
    const uint PRESENCE_MEDIUM = 2u;

    /// And for an instance the bin cannot place by the world camera's tiles: the player's own
    /// arms, which are traced along another camera's rays. Put in every tile.
    const uint PRESENCE_EVERYWHERE = 4u;

    /// Where one instance of those kinds can be met: a sphere about everything it places, in world
    /// space, and the `PRESENCE_` bits it carries. Written by a placement, one row an instance of
    /// either kind, and binned into the screen's tiles by `spriterects.comp` beside the sprites.
    struct GpuPresence
    {
        vec3 mCentre;
        float mRadius;
        uint mKinds;
    };

    struct GpuMaterial
    {
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
        /// **The mode does not survive the trip here either.** `Material::isBlended` settles it on
        /// the host, for the same reason the cutoff is settled there: a surface the content asked a
        /// blend of stores the alpha that blend weighs by, and every other surface stores a one
        /// that nothing has to branch on. What the number means is the blend's — coverage where the
        /// surface covers, strength where it adds.
        ///
        /// Multiplied by the texture's alpha at the candidate, which is what a blend does: a stained
        /// pane's texture says where the lead is and this says how much glass there is.
        float mOpacity;

        /// Where this material's terrain layers are, or a count of zero for a single-textured
        /// surface — which is everything but the ground.
        ///
        /// **The count is also what says a hit is ground.** Only terrain is given layers, and
        /// terrain without one is never made, so nothing else in the row has to state the kind —
        /// what sorts a material otherwise is the instance's shader-table offset, which reaches the
        /// shader as the closest-hit shader that ran.
        uint mLayerOffset;
        uint mLayerCount;

        /// A map of what glows and how much, or `NO_TEXTURE`. Added past the albedo rather than
        /// through it, which is where the original engine adds it.
        uint mEmissive;

        /// What the texture is tinted by. **Three channels and not the material's four**: its alpha
        /// is `mOpacity` above, already resolved against the mode, and a second copy of it here would
        /// be a number the shader never reads.
        vec3 mDiffuseColour;

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

        /// A sphere-mapped sheet added past the albedo, indexed by where the eye is, or
        /// `NO_TEXTURE`; and what it is tinted by. `Rtx::Material::mEnvironment` says what it is.
        uint mEnvironment;
        vec3 mEnvironmentColour;

        /// A map the albedo is multiplied by, or `NO_TEXTURE`, read at the unit `mFlags` names.
        uint mDark;

        /// A tangent-space normal map, or `NO_TEXTURE`. Read at the diffuse's own place on the sheet
        /// — a companion map is attached at a unit with no coordinates of its own, and reads unit
        /// nought's — through the tangents the mesh carries.
        uint mNormal;

        /// A `_spec` map in the metal and roughness layout, or `NO_TEXTURE`: metalness in red,
        /// perceptual roughness in green. **What names it also says the diffuse was authored as an
        /// albedo**, so a material that has one is not delit — `Rtx::SpecularLayout` is what lets a
        /// row carry one at all.
        uint mSpecular;

        /// What this material is that no number above says — the `MATERIAL_*` bits.
        ///
        /// **Last.** A `vec4` is four-aligned in scalar layout like everything else here, so this
        /// costs the row four bytes and pads nothing.
        uint mFlags;
    };

    // **The host's layout has to be the one the device reads**, because this side writes these
    // buffers and the shader reads them — and a padding byte nobody asked for is a mistake that
    // produces a plausible wrong image rather than an error. GLSL is pinned separately, by the
    // `--scalar-block-layout` the build hands the validator.
#ifdef RTX_HOST
    static_assert(sizeof(GpuMesh) == 24, "GpuMesh must be scalar-packed on every side");
    static_assert(sizeof(GpuInstance) == 60, "GpuInstance must be scalar-packed on every side");
    static_assert(sizeof(GpuLight) == 40, "GpuLight must be scalar-packed on every side");
    static_assert(sizeof(GpuLightGrid) == 28, "GpuLightGrid must be scalar-packed on every side");
    static_assert(sizeof(GpuLayer) == 64, "GpuLayer must be scalar-packed on every side");
    static_assert(sizeof(GpuMaterial) == 96, "GpuMaterial must be scalar-packed on every side");
    static_assert(sizeof(GpuSprite) == 56, "GpuSprite must be scalar-packed on every side");
    static_assert(sizeof(GpuEmitter) == 40, "GpuEmitter must be scalar-packed on every side");
    static_assert(sizeof(GpuEmitterFrame) == 16, "GpuEmitterFrame must be scalar-packed on every side");
    static_assert(sizeof(GpuPresence) == 20, "GpuPresence must be scalar-packed on every side");
    static_assert(sizeof(GpuTables) == 184, "GpuTables must be scalar-packed on every side");

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
