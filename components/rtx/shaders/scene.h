// `#pragma once` everywhere else in this tree, and an include guard here: `glslc` warns
// "'#pragma once' : not implemented" and carries on, so a header included twice by one
// shader would redefine everything in it.
#ifndef OPENMW_COMPONENTS_RTX_SHADERS_SCENE_H
#define OPENMW_COMPONENTS_RTX_SHADERS_SCENE_H

#include "portable.h"

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
    using uint64 = std::uint64_t;

#else

// **Asked for here rather than by each shader that includes this.** The tables below are addressed
// by 64-bit pointers, so a shader reading them needs the extension in scope — and nineteen of them
// declared it for themselves while this header, which is what actually spells `uint64_t`, declared
// nothing. A pass that took a constant out of here and had no use for a pointer got a syntax error
// pointing at a line it never wrote.
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

#define uint64 uint64_t

#endif

    /// A slot of the bindless texture array that is not one.
    ///
    /// **Every slot, and not only a material's.** A cloud deck, a star sheet and a moon's face index
    /// the same array a diffuse map does, so what stands for *nothing loaded* is the same value with
    /// the same meaning — and it used to be that value under three names in two headers, which is a
    /// reader having to check that they agreed.
    const uint NO_TEXTURE = 0xFFFFFFFFu;

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

    /// A whole turn, which is how a wavelength becomes a wavenumber.
    const float TAU = 6.2831853f;

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
    /// Exactly the number drawn and not a round one: the fog takes a number, the bounce takes a
    /// pair, and the water's own march takes a number. A spare channel would have to be given a step
    /// to advance by, and the honest step for a stream nobody reads is nothing — which is a value
    /// frozen for the life of the process, waiting for whoever reaches for it next.
    const uint RANDOM_STREAMS = 4;

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
    const uint STREAM_FOG = 0u;
    const uint STREAM_BOUNCE = 1u;

    /// Where the water's shaft march starts inside its first step.
    ///
    /// **Its own channel and not the fog's**, though both are march offsets down one ray: a pixel
    /// whose air started late would have its water start late too, and the two marches lie end to
    /// end along the same line.
    const uint STREAM_WATER = 3u;

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
    /// The figure is what the field this replaced measured at, so the band and `FOG_COVERAGE` keep
    /// the meanings they were set against.
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

    /// How many froxels one workgroup of the scatter pass covers, across the screen and in depth.
    ///
    /// **Two hundred and fifty-six threads, laid out to keep what they read together.** Froxels
    /// beside each other read one block of the fog field and walk the same cells of the light grid,
    /// and froxels behind each other walk the cells of one ray — so both axes are coherent and the
    /// only thing the shape decides is which is more so. Eight by eight keeps the screen-space
    /// block square, which is what the field's read and the reprojection both want.
    const uint FOG_FROXEL_WORKGROUP_ACROSS = 8u;
    const uint FOG_FROXEL_WORKGROUP_DEEP = 4u;

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
    const uint MASK_SOLID = 0x01u;
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

    /// How many see-through surfaces the eye peels off before it draws what is under them.
    ///
    /// **A person is a stack and a window is not.** A pane of glass is one surface, and one layer
    /// answered it; a cuirass over a skirt over a leg is three, so an actor the game fades —
    /// Invisibility, Chameleon, the distance fade at the edge of `actors processing range` — showed
    /// its nearest layer faded and every layer under it at full strength.
    ///
    /// **Four, because that is a dressed person and what is behind them.** The layers are peeled
    /// nearest first and the surface after the last is drawn as the solid it stands in for, so a
    /// deeper stack ends in a surface rather than in a hole. Red Mountain's deepest ray crosses
    /// eight translucent surfaces — `shot --crossings` is the census — and those are the medium's,
    /// which a ray never stops at: `MASK_MEDIUM` says why a shell is gathered rather than met.
    ///
    /// Each layer costs a traversal on the pixels that reach it, and none on a pixel with nothing
    /// see-through in it.
    const uint PEEL_LAYERS = 4u;

    /// A surface that is nowhere opaque, gathered as a depth along the ray rather than met.
    ///
    /// **Carried beside `MASK_SOLID` and not instead of it**, because a medium is still something a
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

    /// The content doubled every triangle of this mesh for its back — `Rtx::FoldedShape::mSheet`.
    /// With a mask on its material that is a leaf, and `SHEET_TRANSMISSION` says what the light on
    /// its far side is worth to it.
    const uint MESH_SHEET = 0x01u;

    /// Every edge of this mesh carries a triangle each way — `Rtx::FoldedShape::mClosed`. It says
    /// which of a surface's two normals describes it, which `litCosine` reads.
    const uint MESH_CLOSED = 0x02u;

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
        /// three words and every ray that lands fetches one.
        uint mShape;
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
    /// **Carried in the frame's block, as `VisibilityConstants::mLightGrid`.** It had a storage
    /// buffer of its own for a while, from when the frame's block was a push constant at the edge of
    /// its 256 bytes; that block is a uniform buffer written once a frame now, and the pass already
    /// folds the sea's tables into it from the passes that built them. A twenty-eight byte record in
    /// a set of its own cost a descriptor, a buffer per frame in flight, and a storage read at every
    /// lamp lookup where the constant bank serves.
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
    /// geometry already is: a descriptor per table was seventeen storage-buffer bindings pushed twice
    /// a frame, and a binding the layout declared and the pass forgot was a shader reading whatever
    /// the slot held. An address is a 64-bit integer, so the struct belongs to the scene and not to
    /// a backend: the Vulkan shader constructs a `buffer_reference` from each.
    ///
    /// **Filled by the pass and not by `describeWorld`**, the way `mWaveExtent` and `mLightGrid`
    /// are: where a table is lives with whatever placed it there, and the tables that alternate by
    /// frame slot change address every frame.
    ///
    /// **No size beside an address.** A descriptor carried one and robust access bounded a read by
    /// it; a pointer carries none. What stops a shader reading past a table is its count, exactly
    /// as before, and what reports one that does is GPU-assisted validation's address table.
    ///
    /// **What it costs, measured on the release harness against the descriptor build**: nothing on
    /// the exteriors, and three per cent of the trace at the Balmora mages' guild — 0.04 ms — in
    /// every one of thirteen interleaved pairs. The compute pipelines compile to byte-identical
    /// sizes either way, so that is a load path and not an instruction count, and the driver shows
    /// no kernel's disassembly. Accepted as the price of six bindings and of a class of mistake
    /// gone; the power-capped card's own drift is of the same size.
    struct GpuTables
    {
        /// The three tables of block addresses, which a global vertex or index id is resolved
        /// through. The normals are this slot's copy.
        uint64 mNormalBlocks;
        uint64 mTexCoordBlocks;
        uint64 mIndexBlocks;

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
        uint64 mSprites;
        uint64 mEmitters;

        /// The sprite tiles' list, in the same shape over the screen's tiles.
        uint64 mSpriteTileList;
    };

    /// What a reference to each table may claim about its address, and so what the host checks.
    ///
    /// **The largest power of two that divides both the buffer's start and every element access.**
    /// A claim larger than the truth is undefined behaviour with no message. A claim smaller than
    /// the truth costs the compiler a wider load where one was possible. A buffer's start is at
    /// least sixteen-aligned on this device and the host asserts it, so the stride decides:
    /// `GpuLayer` is 48 bytes with two `vec4` at sixteen and thirty-two, the block tables hold
    /// eight-byte addresses, and every other row or list is four-aligned only.
    const uint TABLE_ALIGN_ROWS = 4u;
    const uint TABLE_ALIGN_BLOCKS = 8u;
    const uint TABLE_ALIGN_LAYERS = 16u;

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
    /// frame's extent on both sides — `spriteTilesOver` — so there is one number here and no second
    /// one to disagree with it.
    ///
    /// **Eight was measured twice and it is a loss both times.** Against the host bin, over Balmora
    /// at night in the rain, four times the tiles took 0.32 ms off the trace and put 2.4 ms on the
    /// frame, because the offsets were one entry per tile written across the bus every frame.
    /// Against the device bin the offsets cost nothing and the trace gained nothing at all — 1.10
    /// ms at either size, since a drop's own test is cheap once the lamps are walked per emitter —
    /// while the fill, which walks every sprite for every tile, went from 0.05 ms to 0.17.
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
    /// count in entry one, and the trace walks every sprite over every pixel for that frame: the
    /// march as it was before the tiles, slow and right. The host reads what the frame needed,
    /// grows the buffer and the next frame is binned. `SceneBuffers::binSprites` says how the list
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
        /// is `mOpacity` above, already resolved against the mode, and a second copy of it here was
        /// a number the shader never read.
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

        /// What this material is that no number above says — `MATERIAL_MEDIUM` and nothing else yet.
        ///
        /// **Last, so the row's every other field stays where it was.** A `vec4` is four-aligned in
        /// scalar layout like everything else here, so this costs the row four bytes and moves
        /// nothing.
        uint mFlags;
    };

    // **The host's layout has to be the one the device reads**, because this side writes these
    // buffers and the shader reads them — and a padding byte nobody asked for is a mistake that
    // produces a plausible wrong image rather than an error. GLSL is pinned separately, by the
    // `--scalar-block-layout` the build hands the validator.
#ifdef RTX_HOST
    static_assert(sizeof(GpuMesh) == 12, "GpuMesh must be scalar-packed on every side");
    static_assert(sizeof(GpuInstance) == 60, "GpuInstance must be scalar-packed on every side");
    static_assert(sizeof(GpuLight) == 36, "GpuLight must be scalar-packed on every side");
    static_assert(sizeof(GpuLightGrid) == 28, "GpuLightGrid must be scalar-packed on every side");
    static_assert(sizeof(GpuLayer) == 48, "GpuLayer must be scalar-packed on every side");
    static_assert(sizeof(GpuMaterial) == 68, "GpuMaterial must be scalar-packed on every side");
    static_assert(sizeof(GpuSprite) == 56, "GpuSprite must be scalar-packed on every side");

    static_assert(sizeof(GpuEmitter) == 60, "GpuEmitter must be scalar-packed on every side");
    static_assert(sizeof(GpuTables) == 112, "GpuTables must be scalar-packed on every side");

#endif

#ifdef RTX_HOST
}
#else
#undef uint64
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
