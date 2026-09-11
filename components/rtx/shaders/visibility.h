// `#pragma once` everywhere else in this tree, and an include guard here: `glslc` warns
// "'#pragma once' : not implemented" and carries on, so a header included twice by one
// shader would redefine everything in it.
#ifndef OPENMW_COMPONENTS_RTX_SHADERS_VISIBILITY_H
#define OPENMW_COMPONENTS_RTX_SHADERS_VISIBILITY_H

#include "camera.h"
#include "hosttypes.h"
#include "portable.h"
#include "scene.h"
#include "sky.h"
#include "wave.h"

// Included verbatim by both the shader and the C++ that fills it in, so the two cannot disagree
// about a field. Scalar block layout is what makes that possible: a `vec3` is twelve bytes on both
// sides, with none of the padding rules that make std140 a translation exercise.

// `<cstddef>` for the `offsetof` the pinned layout below is checked with, which is the one thing
// this header needs that `hosttypes.h` does not carry.
#ifdef RTX_HOST

#include <cstddef>

namespace Rtx::Shaders
{
#endif

    /// How many closest-hit shaders the trace has: one for each `Rtx::MaterialKind`, in the order
    /// that enum names them, each standing behind `HIT_RECORD_LAYERS` records of the shader binding
    /// table.
    ///
    /// **The record is the kind and the layer, and traversal follows it.** `SceneAcceleration::placeRow`
    /// writes each instance's shader-table offset from its material's kind, so the hardware follows
    /// an index to the shader rather than the shader reading a material row to find out what it is —
    /// and the launch adds the layer it is tracing for, so the shader reads that off its record
    /// rather than off the payload.
    const uint HIT_SHADER_COUNT = 3u;

    /// What a hit record carries after its handle, which is everything a closest-hit shader is told
    /// by the launch that invoked it.
    ///
    /// **Nothing crosses the payload inwards, and this is why.** A field the launch writes into the
    /// payload before `hitObjectExecuteShaderEXT` is not what the closest-hit shader reads once a
    /// `reorderThreadEXT` with a key stands anywhere in the launch — measured on driver 610.57.04,
    /// where a per-pixel signature written that way arrived wrong at 99.5 per cent of pixels and the
    /// answers written back arrived right at every one. No launch here sorts today, and the record
    /// is what keeps that a choice: it is read by the shader the hit object names, through the index
    /// traversal computed, whatever stands between the trace and the execute.
    struct HitRecord
    {
        /// Which layer of the peel the shader is standing at, counting the eye's own hit as nought.
        /// `PEEL_LAYERS` is the one past the last pane the launch peels, and a surface found there is
        /// drawn as the solid it stands in for whatever its own opacity says.
        uint mLayer;
    };

    /// The sky, which is the only miss record the trace has.
    const uint MISS_RECORD_SKY = 0u;
    const uint MISS_RECORD_COUNT = 1u;

    /// What the frame is: where the eye stands, how it turns a pixel into a ray, and everything
    /// about the world that a ray needs to be answered.
    ///
    /// **The ray generator is `Camera` and is separate**, because the wavelet builds the same rays
    /// and needs none of the rest of this. What is left here is the world.
    struct VisibilityConstants
    {
        vec3 mOrigin;

        /// How a pixel becomes a ray. The eye's own place is `mOrigin` above and not in here, for
        /// the reason `Camera` gives.
        Camera mCamera;

        /// Where the depth buffer's zero sits, in world units from the eye.
        ///
        /// **A ray tracer has no near plane and an upscaler asks for one anyway.** Nothing here
        /// clips against it; it exists so the depth written for DLSS is the value a rasterizer with
        /// this frustum would have written, which is what NGX's disocclusion test expects to be
        /// looking at.
        float mNear;

        /// How far a ray travels before whatever it was looking for counts as not being there.
        ///
        /// The world's own size, near enough: a primary ray that reaches this has left it, and so
        /// has the sun's shadow ray, which is the same question asked from the other end.
        float mFar;

        /// Non-zero to write the albedo straight out, with no shading over it. What a test asserting
        /// "this pixel is that texel" needs, and what makes a texture problem visible as itself.
        uint mShowAlbedo;

        /// Non-zero where there is no sky behind the subject: a ray that hits nothing comes back
        /// with no radiance **and no coverage**, so the pixel is transparent rather than the
        /// horizon's colour.
        ///
        /// **What a picture inside the interface is.** The inventory doll and a map tile are
        /// composited over the window behind them rather than filling it, so what they do not cover
        /// has to be nothing at all. Zero is a frame that fills a window, where the sky is the
        /// answer and every pixel is opaque.
        uint mTransparentBackground;

        /// Where the sun stands, unit, and how much of its light arrives on a surface square to it.
        ///
        /// One directional light, handled apart from the point lights because it has no position and
        /// no falloff: it is the same everywhere and its shadow ray runs to the end of the world.
        ///
        /// **One vector and not two**, so `-mSunPosition` is where the light travels. The game gives
        /// its light a fixed climb and its disc a height of `swing - |east|`; a rasterizer can hold
        /// both, and a tracer answering to each in turn gets a different sun in the shadows, the
        /// water and the haze.
        ///
        /// **And one test for whether there is a sun at all: `mSunIrradiance` is zero.** An
        /// interior, a night, and either end of the day once the disc has gone into the horizon all
        /// say it that way, and every use of the sun below is gated on it — the shadow ray, the
        /// caustics, the shafts and the disc. There is deliberately no second field saying whether
        /// the disc is drawn; `Rtx::makeSkylight` is where that is kept true and why.
        ///
        /// The position stays meaningful through the night even so, because a moon's crescent points
        /// at where the sun would be. Where it is and whether it is there are separate questions.
        vec3 mSunPosition;
        vec3 mSunIrradiance;

        /// What the disc is painted with, linear.
        ///
        /// **The hue is not the sunlight's.** What a weather gives its sunlight is the sky's colour
        /// as much as the sun's — `Sun_Night_Color` is a blue no sun ever was — and the ramp is
        /// still crossing to it through the whole of dawn, so a disc tinted by the light comes up
        /// blue. Morrowind records the disc's own colour and it is white until the sun starts down.
        /// How much of it there is is not here: the irradiance already carries that, which is what
        /// makes a drawn disc and a cast shadow the same fact.
        vec3 mSunDiscColour;

        /// The cloud deck and the star field over it, and the nebulae and constellations painted
        /// across that. They all fade together on `StarField::mFade`, because in the engine they are
        /// one mesh under one switch.
        CloudDeck mClouds;
        StarField mStars;
        SkyPatch mSkyPatches[SKY_PATCH_COUNT];

        /// What a ray that hits nothing comes back with, at the horizon and overhead.
        ///
        /// The game's own two colours: its atmosphere is the one overhead and its fog is what that
        /// fades to at the horizon, which is most of what a Morrowind sky is.
        vec3 mSkyHorizon;
        vec3 mSkyZenith;

        /// How much of `mAmbient` arrives from the sky, from none of it to all.
        ///
        /// **The two things an ambient can be, told apart.** Out of doors it is the sky. Inside it
        /// is the cell's own `AMBI`, a fill standing for every bounce the room makes. Two answers
        /// hang off which one it is.
        ///
        /// **Whether the sky is a light at all.** A room's dome is its fog colour standing in for
        /// the picture wherever a ray leaves the shell, and lighting anything with that drew a
        /// bright band along the foot of every wall. Nought here, and a bounce that reached nothing
        /// brings back nothing.
        ///
        /// **How far `ambientReaching` looks for what stands over a point.** Out of doors the
        /// ambient is the sky and the ray runs to it, so anything at all takes it away. In a room
        /// the walls *make* the fill rather than block it, so only what is within
        /// `ROOM_FILL_REACH` does — a ray run out to the walls comes back blocked everywhere and
        /// empties every interior, and occluding by nothing at all let white cloth light its own
        /// contact shadow.
        ///
        /// **A frame assembled by hand is a room until it says otherwise**, since nothing else in
        /// the constants says which a cell is. `Rtx::describeWorld` writes this on every frame
        /// either host renders, so only a test builds one that has not.
        ///
        /// One and nought are the only values either host writes today, and a fraction is what a
        /// cell part open to the sky would want.
        float mAmbientFromSky;

        /// What the sky lights with over and above those two, and is not drawn with.
        ///
        /// **The one place where what the sky sends and what the sky shows are different things.**
        /// `Rtx::skyFill` carries the whole of why: Morrowind states a night's light as an ambient
        /// on every surface, which is an order above the colour it draws its night sky, and a
        /// renderer that lights the ground by tracing that sky is short by the difference.
        vec3 mSkyFill;

        /// Where the water's surface is, or negative infinity where the cell holds none.
        ///
        /// Infinity rather than a flag: everything that asks does so as "how deep is this point",
        /// and a level of minus infinity makes that never positive, so a cell with no water takes
        /// the same path as a point above the surface with no branch of its own.
        float mWaterLevel;

        /// How long the water has been moving, in seconds.
        ///
        /// Zero is a still sea and a deterministic frame, which is what a test wants; the window
        /// path passes its own clock.
        float mTime;

        /// How hard it rains on the water, from nought to one.
        ///
        /// **The precipitation's own alpha where its kind rings the surface, and nought where it
        /// does not** — which is the number the rasterizer hands its water as `rainIntensity`, and
        /// `Weather::Precipitation::ripplesEnabled` is what says whether a kind rings: rain does and
        /// snow settles, off the ini's own `Rain Ripples` and `Snow Ripples`. `rainSlope` is what
        /// reads it.
        float mRainOnWater;

        /// Which way the wind drives the sea, unit, in the world's XY.
        ///
        /// **The tiles are spread about their own +X, and this is what turns them.** The wind's
        /// heading changes with the weather and turns through a transition, and a spectrum built
        /// for one heading would have to be built again for the next; a rotation of where the tiles
        /// are sampled costs nothing and is exact. It is the deck's heading, because there is one
        /// wind over a landscape — the same one the fog is carried by — and +X where nothing blows,
        /// which is a sea that runs as its tiles were drawn.
        vec2 mSeaHeading;

        /// How wide each of the sea's tiles is, in world units, in the order they are bound.
        ///
        /// **What turns a world position into a texture coordinate and a cone width into a level.**
        /// The tiles are a compile-time table and could have been a shader constant; they are handed
        /// over instead so what the shader divides by is what `WavePass` actually built, rather than
        /// a second copy of it that a change to the first would leave behind.
        float mWaveExtent[WAVE_CASCADES];

        /// How wide one texel of each of those tiles is, in world units, in the same order.
        ///
        /// **Handed over rather than asked of the driver.** `waveLevel` divides a footprint by this
        /// to reach a level of the chain, and it read the grid with `textureSize` — a texture-header
        /// fetch for a number `sWaveTiles` states at compile time, twice a cascade, in a function an
        /// underwater pixel calls six times over. The extent above is already written from the tile
        /// that built it, so this is the same statement one division further on.
        float mWaveTexel[WAVE_CASCADES];

        /// Root mean square slope of the whole sea, over every tile and every wavelength in them.
        ///
        /// **A property of the sea and not of a place in it**, which is why it is one number and not
        /// a fetch. What wants it is the caustic's band limit: a point at depth `d` gathers its light
        /// from a patch of surface `bend * d * this` across, so past a few metres the pattern is
        /// blurred by the surface's own spread of slopes rather than by the pixel looking at it.
        /// Reading it off the coarsest level of a chain would be two texture fetches at every step of
        /// a march, for a number that is the same at all of them.
        float mWaveSlope;

        /// Mean square of the sea's curvature trace, over every tile and every wavelength in them.
        ///
        /// What the caustic's `bend` is sized against, so that `WATER_CAUSTIC_FOLD` says how far the
        /// map runs whatever sea state the weather asked for. **A property of the sea and not of a
        /// place**, for the reason `mWaveSlope` gives.
        float mWaveCurvature;

        /// What share of `mWaveCurvature` a tile still resolves at a level of its chain, indexed
        /// `cascade * WAVE_LEVELS + level`.
        ///
        /// **The fold the caustic's gain is read at, and two things were wrong with taking it off
        /// the chain.** It was differenced per pixel — the tile's whole mean square less what the
        /// footprint's own averaged away — so it was noisy, and its noise ran with the very
        /// determinant it was normalising. And a chain read that way answers for the *texels*, where
        /// what the shader sees is `textureLod` reconstructing between them: a second filter, and a
        /// large one, that costs a quarter of the curvature in the shallows and two thirds of it in
        /// deep water. `Rtx::waveCurvature` states both filters over the amplitudes, and the chain
        /// this replaced has no reader left.
        float mWaveResolved[WAVE_CASCADES * WAVE_LEVELS];

        /// The cell's own ambient, linear, and what a path is terminated with.
        ///
        /// **No longer added on top of the light that is traced, which is what it used to be.**
        /// Morrowind's interiors were authored against a renderer with no bounce at all, so this
        /// term stood in for every one of them; adding it to a surface that now gathers a real
        /// hemisphere would count the same light twice. It sits one level down instead — a bounce
        /// that lands on something is shaded with this rather than gathering a hemisphere of its
        /// own, so it estimates the rest of a path nobody traces.
        ///
        /// **It is load-bearing indoors and marginal outdoors.** Measured from inside the Balmora
        /// mages' guild, zeroing it halves the frame: 0.0033 mean luminance to 0.0016. Over Balmora
        /// itself it is worth 1.8%, because an exterior's second bounce mostly finds sky, which is
        /// traced for real.
        vec3 mAmbient;

        /// What the air between the eye and everything else scatters toward it, and how much of it
        /// there is.
        ///
        /// **The colour is the horizon's**, and not by coincidence: Morrowind records one colour for
        /// the fog and the sky's lower half because they are the same thing seen at two distances,
        /// which is why a ray that reaches nothing has to converge on exactly what a ray through a
        /// mile of air does. An interior carries its own in `AMBI` instead.
        ///
        /// The extinction is absolute, per world unit, at the fog's base: the host has already
        /// turned the record's view-range-relative dial into one. Zero is no fog at all and costs
        /// nothing — which is what the tests that measure surface radiance need, since a lit surface
        /// with fog over it is a differently lit one.
        vec3 mFogColour;
        float mFogExtinction;

        /// One where the air is an even haze, zero where it is banked.
        ///
        /// **A room is not a small valley.** Banks are what weather does to a landscape, and a cell
        /// smaller than one bank running the outdoor coverage field reads as a rendering fault
        /// rather than as weather. The two are mixed rather than branched, so a cell can be anywhere
        /// between — and because the banked field is normalised to average one, moving along that
        /// mix changes the air's character and never how much of it there is.
        float mFogUniform;

        /// How deep the fog's layer stands, as a multiple of `FOG_HEIGHT`.
        ///
        /// **A weather with more fog has fog that reaches higher, and the game says so.** `Land Fog
        /// Depth` is called depth for a reason: Morrowind writes 0.69 for clear and 1.9 for a foggy
        /// night, so foggy's air fills a bay where clear's lies in the hollows. Without this every
        /// weather pooled in the same 37-metre bank, and a medium that filled the sky while doing
        /// that was two answers to one question.
        ///
        /// `Rtx::fogLift` is what derives it, and says why the wind alone could not.
        float mFogLift;

        /// Which way the air is moving and how fast, in the ground plane, as a heading times the
        /// weather's recorded wind.
        ///
        /// **Advection, which is not what `FOG_CHURN` is.** The churn drags the scales past each
        /// other on headings that disagree, which is what makes the shapes form and pull apart —
        /// air doing that in a dead calm is the whole reason a still fog is not a frozen texture.
        /// This is the separate thing a wind adds: the entire field carried downwind together, on
        /// the heading the cloud layer drifts along, because there is one wind over a landscape and
        /// cloud shadows crossing the ground one way while the air moves another would read as two
        /// weathers at once. `FOG_GALE` says what a unit of it is worth.
        vec2 mFogWind;

        /// How far from the eye the world is built, in units. Zero where nothing is cut off.
        ///
        /// **The second element of the air, and the one the weather knows nothing about.** Morrowind
        /// records how thick its own fog is and this path honours that record; what it cannot record
        /// is where this renderer stopped building ground, so the last cell ends in mid-air and the
        /// player sees the cut. The air here closes over that ring and over nothing nearer —
        /// `FOG_EDGE_RAMP` and `FOG_EDGE_RISE` are the shape of it.
        float mFogEdge;

        /// Masser and Secunda, in that order. An interface trace and an interior leave both at an
        /// alpha of nothing, which costs the sky one compare each.
        MoonDisc mMoons[MOON_COUNT];

        /// Where the scene's lamps were binned. The pass folds it in from the tables it is handed,
        /// the way it folds the sea's in — `GpuLightGrid` says why it rides here and not in a table.
        GpuLightGrid mLightGrid;

        /// How much of each texture's painted-in lighting to divide back out, from zero to one.
        ///
        /// **Morrowind's textures were lit before they were saved**, and a ray tracer lights them
        /// again — so a corner with occlusion painted into it is dark twice over. One is the whole
        /// estimate and zero is the A/B that says what it did.
        float mDelight;

        /// Where the eye stands now, less where it stood on the previous frame.
        ///
        /// **Differenced on the host, and that is the whole trick.** Morrowind's coordinates run to
        /// six figures and a motion vector is a fraction of a pixel, so subtracting two world points
        /// on the device throws the answer away in rounding. Two camera positions within a step of
        /// each other subtract exactly in a float, and the device only ever adds that small delta to
        /// an offset from its own eye.
        vec3 mCameraMotion;

        /// The previous frame's basis, in the same form as `mForward`, `mRight` and `mUp`, with the
        /// translation left out — it is `mCameraMotion` that carries where the eye was.
        ///
        /// All zero before there is a previous frame, which the shader reads as "no answer" and
        /// leaves the motion at nothing.
        vec3 mPreviousForward;
        vec3 mPreviousRight;
        vec3 mPreviousUp;

        /// Which frame this is, for anything that wants a different answer than last time.
        ///
        /// Every random draw in the shader is keyed on it — the fog's step jitter and the bounce's
        /// direction — so it is what makes two renders of one camera differ. Zero is a repeatable
        /// frame, which is what a test wants; a window passes its own count.
        uint mFrame;

        /// Non-zero where something after the trace composites the transparency layer.
        ///
        /// **Which is Ray Reconstruction and nothing else.** The sprites reach `CHANNEL_TRANSPARENCY`
        /// either way, and this says whether anybody will read it: the upscaler is handed it as
        /// `pInTransparencyLayer` and composites it with a motion vector of its own, and every other
        /// path — the upscaler off, a doll, a map tile, a test that reads the frame — has nothing
        /// that would. Those composite it in the trace instead and get the frame they always had.
        ///
        /// **Here because this is where the padding was.** `mTables` below is eight-aligned and
        /// everything above is four, so both languages left four bytes idle in front of it and this
        /// took them. The flag under it found none left and grew the struct by its own eight, which
        /// is what the two asserts below now pin.
        uint mLayerCompositedAfter;

        /// Non-zero where this scene holds a surface the eye passes through — a cloud's shells.
        ///
        /// **What keeps `mediumAlong` out of every frame that has none.** The walk traverses on
        /// `MASK_MEDIUM`, and where nothing carries that bit it still descends the top level once a
        /// pixel to find nothing: 0.02 ms of a 1.86 ms trace over Seyda Neen. One uniform branch
        /// takes it back, and the frames it takes it back from are most of the game.
        uint mMediumInFrame;

        /// How many columns and rows of froxels stand in front of the camera.
        ///
        /// **The froxel grid, said once, where three shaders asked the driver for it.** `puffLight`
        /// queried the volume's size once per covering sprite, `fogVolumeAlong` once per pixel and
        /// `fogVolumeWas` once per froxel — three `textureSize` calls for one pair of integers
        /// `FogVolume` holds on the host and never changes within a frame.
        uvec2 mFogColumns;

        /// Where every table a hit reads is. `GpuTables` says why it rides here.
        ///
        /// **Last, because it is eight-aligned and nothing before it is.** Anywhere else it would
        /// pad the middle of a struct two languages have to agree on, and the offset asserted below
        /// pins where it landed.
        GpuTables mTables;
    };

    // Pinned for the reason `scene.h` gives: the side that writes these bytes and the side that
    // reads them are different compilers.
#ifdef RTX_HOST
    static_assert(offsetof(VisibilityConstants, mTables) == 1008, "GpuTables must land where the padding was");
    static_assert(sizeof(VisibilityConstants) == 1128, "VisibilityConstants must be scalar-packed on every side");

#endif

#ifdef RTX_HOST
}
#endif

#endif
