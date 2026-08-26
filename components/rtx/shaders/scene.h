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

    /// How many sinusoids the water surface is summed from.
    RTX_CONST uint WAVE_COUNT = 32u;

    /// A whole turn, which is how a wavelength becomes a wavenumber.
    RTX_CONST float TAU = 6.2831853f;

    /// Morrowind's gravity, in world units per second squared: 8.96 m/s^2 across 69.99 units to
    /// the metre.
    RTX_CONST float WATER_GRAVITY = 627.1f;

    /// One sinusoid of the sea, as the shader reads it.
    ///
    /// **One height field, differentiated twice.** The normal is its gradient and the caustics are
    /// its curvature, so the two cannot disagree about where a crest is — which they would the
    /// moment either sampled a field of its own.
    struct GpuWave
    {
        /// Unit vector the wave travels along.
        vec2 mDirection;

        /// Radians of phase per world unit.
        float mWavenumber;

        /// Half the crest-to-trough height, in world units.
        float mAmplitude;

        /// Radians of phase per second, from the dispersion relation at this depth.
        float mSpeed;
    };

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
    /// Exactly the number drawn and not a round one: the fog takes a number and the bounce takes a
    /// pair. A spare channel would have to be given a step to advance by, and the honest step for a
    /// stream nobody reads is nothing — which is a value frozen for the life of the process, waiting
    /// for whoever reaches for it next.
    RTX_CONST uint RANDOM_STREAMS = 3;

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

    /// Angular radius of the sun, in radians — a disc about half a degree across.
    ///
    /// The real figure, because there is only one right answer and nothing about this renderer wants
    /// a different sun. It decides how wide the disc in the sky is drawn, and with it how wide the
    /// glitter path on water is: the two are the same number seen twice, one directly and one in a
    /// mirror, and they cannot be allowed to disagree.
    RTX_CONST float SUN_ANGULAR_RADIUS = 0.004654f;

    /// What an emissive of one is worth, as light.
    ///
    /// **The original's scale is not this renderer's.** There a fully lit surface reached one and an
    /// emissive of one matched it; here the direct sun is `DAYLIGHT`, so the same number has to be
    /// carried across or a glow that read as bright becomes a rounding error. Matched to the sky
    /// rather than to the sun, which is about a fifth of it: what these materials are for is being
    /// visible in shade, and a glowing mushroom is not as bright as the sun on it.
    RTX_CONST float EMISSIVE_INTENSITY = DAYLIGHT * 0.2f;

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
    /// **Red goes first and the rest goes slowly**, which is the one thing about water's colour that
    /// is not a matter of taste and is why shallow water reads green where deep water reads blue.
    /// Jerlov's coastal type, quoted per metre as 0.32, 0.05 and 0.08, over the game's 69.99 units
    /// to the metre. Every expectation a test makes about water derives from this, so a tuning pass
    /// is one line rather than five pieces of arithmetic that quietly stop describing the shader.
    RTX_CONST vec3 WATER_EXTINCTION = vec3(0.004572f, 0.000714f, 0.001143f);

    /// What a raft of bubbles sends back, as a share of what falls on it.
    ///
    /// Koepke measured a whitecap at an effective 0.22 averaged over the whole patch and the decaying
    /// tail behind it; fresh foam itself runs 0.5 to 0.6. What this covers is the fresh part, because
    /// the tail is what the coverage below is already fading out. **Spectrally flat**, which almost
    /// nothing else in this game is: a bubble raft scatters by geometry rather than by pigment, so
    /// foam is the one white surface in a frame where every other white came off a texture.
    RTX_CONST float WATER_FOAM_ALBEDO = 0.55f;

    /// The share of its own depth a wave's height reaches before it breaks.
    ///
    /// McCowan's solitary-wave limit, 1894, and still the number a surf zone is placed with. It is
    /// what makes the foam band's width a consequence of the sea state rather than a distance
    /// somebody picked: a calmer sea breaks closer in, in a narrower strip, with nothing tuned.
    RTX_CONST float WATER_BREAKER_RATIO = 0.78f;

    /// How long a raft of broken water lasts before it has dispersed, in seconds.
    ///
    /// **This is what stops a puddle being surf.** The ratio above is a statement about a wave that
    /// arrives: it says where one breaks, and not whether one ever got here. A hollow inland of the
    /// shore that dips below sea level satisfies it across the whole of itself — every part of it is
    /// shallower than anything breaks in — so it comes out white from edge to edge, which is what a
    /// level pan of water four centimetres deep is not.
    ///
    /// What separates the two is how far the wave had to travel through breaking-depth water to
    /// reach the point, and the bed's own gradient is what says: a few metres where the ground drops
    /// away to open sea, tens of metres across a pan that does not drop at all. Broken water is
    /// carried shoreward at the shallow-water celerity and thins while it goes, so that distance
    /// becomes a share by way of one time — Monahan and Woolf's decay constant for a whitecap.
    ///
    /// **A time and not a distance, for the reason the ratio above is a ratio.** The length it
    /// stands for is `sqrt(g h)` times this, so a heavier sea carries its foam further by itself,
    /// and the surf zone stays a consequence of the sea state rather than a band somebody sized.
    RTX_CONST float WATER_FOAM_LIFETIME = 3.5f;

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
    /// four standard deviations. `SeaState` normalises its spectrum to it and the surf line is
    /// placed by it, so it is one number rather than a four written twice.
    RTX_CONST float WATER_SIGNIFICANT_HEIGHT = 4.0f;

    /// The single-scattering albedo: the share of extinction that was scattering and not absorption,
    /// and so the part the water hands back as its own colour instead of swallowing.
    ///
    /// **This is what decides whether deep water is dark.** A channel whose scattering albedo
    /// approaches one settles at a bright colour however deep it gets — a milky sheet. Clear
    /// tropical water really does behave that way, because molecular scattering dominates its blue;
    /// a tannin-stained coastal swamp does not, and this game's water is the second.
    RTX_CONST vec3 WATER_SCATTER = vec3(0.012f, 0.042f, 0.040f);

    /// Which instances a ray is interested in.
    ///
    /// **Water must not cast a shadow, and the mask is how traversal is told so at no cost.** The
    /// alternative — building water non-opaque so the candidate loop can wave shadow rays past — was
    /// measured at half the frame rate, because every shadow ray crossing the sea then invokes a
    /// shader where traversal alone had been enough.
    RTX_CONST uint MASK_SOLID = 0x01u;
    RTX_CONST uint MASK_WATER = 0x02u;

    /// Where a mesh's vertices and indices begin in the shared buffers.
    ///
    /// Indices are mesh-local, so a triangle's vertex is `mVertexOffset` plus what the index says.
    struct GpuMesh
    {
        uint mVertexOffset;
        uint mIndexOffset;
    };

    struct GpuInstance
    {
        uint mMesh;
        uint mMaterial;

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

        /// How big the glowing part is, in world units — which is the whole of what its shadows are
        /// soft by. Zero is a point, and a point casts an edge.
        ///
        /// **Read for visibility and not for radiometry.** The falloff above is a point light's and
        /// stays one, because the air and a puff of smoke evaluate the same falloff and neither can
        /// sample an area; what this changes is where the one shadow ray leaves from.
        float mRadius;
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
    };

    /// How many pixels a side one tile of the sprite list covers.
    ///
    /// **Sixteen, and the trade is the usual one.** Finer tiles reject more sprites per pixel and
    /// cost more of them to bin: a raindrop is a few pixels across, so at sixteen it lands in one
    /// tile or four, and a tile's list is short. The screen's tile count is derived from this and the
    /// frame's extent on both sides — `(width + SPRITE_TILE - 1) / SPRITE_TILE` — so there is one
    /// number here and no second one to disagree with it.
    RTX_CONST uint SPRITE_TILE = 16u;

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
    static_assert(sizeof(GpuWave) == 20, "GpuWave must be scalar-packed on every side");
    static_assert(sizeof(GpuMesh) == 8, "GpuMesh must be scalar-packed on every side");
    static_assert(sizeof(GpuInstance) == 56, "GpuInstance must be scalar-packed on every side");
    static_assert(sizeof(GpuLight) == 32, "GpuLight must be scalar-packed on every side");
    static_assert(sizeof(GpuLightGrid) == 28, "GpuLightGrid must be scalar-packed on every side");
    static_assert(sizeof(GpuLayer) == 48, "GpuLayer must be scalar-packed on every side");
    static_assert(sizeof(GpuMaterial) == 68, "GpuMaterial must be scalar-packed on every side");
    static_assert(sizeof(GpuSprite) == 48, "GpuSprite must be scalar-packed on every side");
    static_assert(sizeof(GpuEmitter) == 56, "GpuEmitter must be scalar-packed on every side");
#endif

#ifdef RTX_HOST
}
#endif

#endif
