// `#pragma once` everywhere else in this tree, and an include guard here: `glslc` warns
// "'#pragma once' : not implemented" and carries on, so a header included twice by one
// shader would redefine everything in it.
#ifndef OPENMW_COMPONENTS_RTX_SHADERS_VISIBILITY_H
#define OPENMW_COMPONENTS_RTX_SHADERS_VISIBILITY_H

#include "camera.h"
#include "portable.h"
#include "scene.h"
#include "wave.h"

// Included verbatim by both the shader and the C++ that fills it in, so the two cannot disagree
// about a field. Scalar block layout is what makes that possible: a `vec3` is twelve bytes on both
// sides, with none of the padding rules that make std140 a translation exercise.

#ifdef RTX_HOST

#include <cstdint>

#include <osg/Vec2f>
#include <osg/Vec3f>
#include <osg/Vec3ui>

namespace Rtx::Shaders
{
    using vec2 = osg::Vec2f;
    using vec3 = osg::Vec3f;
    using uvec3 = osg::Vec3ui;
    using uint = std::uint32_t;

#endif

    /// Threads along each edge of a workgroup.
    ///
    /// The shader declares its local size from this and the dispatch rounds the image up to it, so
    /// the two cannot drift: writing the number twice is how a pass quietly stops covering its last
    /// row of pixels.
    RTX_CONST uint VISIBILITY_WORKGROUP = 8;

    /// Morrowind's ten weathers, in the order `MWWorld::WeatherManager` registers them.
    ///
    /// That order is not an arrangement of this renderer's: it is what a weather's script id counts
    /// along, and it is the order the `Weather_<name>_*` keys sit in a content file. Naming them
    /// here is what lets the game hand over a script id and the harness a name off a command line
    /// and have the two mean one sky — `Rtx::weatherIndex` is the table that joins them.
    RTX_CONST uint WEATHER_CLEAR = 0u;
    RTX_CONST uint WEATHER_CLOUDY = 1u;
    RTX_CONST uint WEATHER_FOGGY = 2u;
    RTX_CONST uint WEATHER_OVERCAST = 3u;
    RTX_CONST uint WEATHER_RAIN = 4u;
    RTX_CONST uint WEATHER_THUNDERSTORM = 5u;
    RTX_CONST uint WEATHER_ASHSTORM = 6u;
    RTX_CONST uint WEATHER_BLIGHT = 7u;
    RTX_CONST uint WEATHER_SNOW = 8u;
    RTX_CONST uint WEATHER_BLIZZARD = 9u;
    RTX_CONST uint WEATHER_COUNT = 10u;

    /// Morrowind's cloud deck, as a ray that reached nothing finds it.
    ///
    /// **A layer at a height rather than the dome the game shipped.** The engine hangs its clouds on
    /// a mesh whose UVs were painted into the file, which is a thing to rasterize and not a thing to
    /// intersect; what that mesh is *for* is a layer of cloud seen in perspective, and a ray tracer
    /// can have the layer itself. Everything here is the game's own, the shape of that layer
    /// included: `CloudShell` is where its height and its curvature are read off the mesh.
    struct CloudDeck
    {
        /// How much deck there is, from none of it to all.
        ///
        /// **Nought is no sky at all, and it is nought by default.** A `VisibilityConstants` is a
        /// plain C structure shared with the shader and has no constructor to run, so whatever means
        /// "there is nothing here" has to be what zeroing it says — a texture index cannot, because
        /// zero is a real slot and every frame that forgot to say otherwise would draw slot nought
        /// across its whole sky. `StarField::mFade` is the same field for the same reason.
        float mOpacity;

        /// What a cloud in full sunlight radiates from below, linear.
        ///
        /// **The sky, the moons and the sun, each spread over the underside of the layer**, and
        /// `Rtx::deckLight` is where the three are added. `CLOUD_TRANSMISSION` is what a deck keeps
        /// of them.
        vec3 mLit;

        /// What a cloud in its own shadow radiates: the sky alone.
        ///
        /// **The light with no direction is the light a cloud cannot shadow itself from.** A deck's
        /// own body is what keeps the sun off its base, so the sheet's paint picks between this and
        /// `mLit` — and at night, with no sun over the layer, the two differ only by the moons.
        vec3 mShadowed;

        /// The mean luminance of what the sheets being sampled paint, linear.
        ///
        /// **What a texel is read as a ratio to, so the sheet gives shape and `mColour` gives the
        /// level.** `SkyContent::mCloudMean` carries the argument and the measurements.
        ///
        /// Nought where the sheet could not be averaged — a file a mod replaced with something
        /// nothing here decodes — which the shader reads as no ratio to take, and draws the deck
        /// flat as it did before it read the paint at all.
        float mMean;

        /// The mean alpha of the sheets being sampled: how much sky the deck hides on average.
        ///
        /// **What a shadow is measured against**, so that darkening the ground states the pattern
        /// and not the weather — `CLOUD_SHADOW_DEPTH` carries the argument.
        float mCover;

        /// Where the layer stands, as a world height, and how many tiles of its sheet one world unit
        /// is along each axis.
        ///
        /// **Signed, because the mesh's own unwrap is.** `CloudShell::mTiles` comes off the cloud
        /// mesh with its `v` axis running the other way, and dropping that sign mirrors every
        /// sheet.
        ///
        /// **The one number in the sky that is chosen rather than read**, and `Rtx::sCloudAltitude`
        /// says so: the mesh gives its height in tiles of its own sheet and no metre anywhere. It is
        /// what lets the sheet be addressed from where the eye stands rather than from where it
        /// looks, and so what lets the deck cast.
        float mAltitude;
        vec2 mPerTile;

        /// How far from `mTexture` to `mNext`. A settled sky names the same texture twice at zero,
        /// so the shader mixes unconditionally rather than testing for a transition.
        float mBlend;

        /// The scroll along `v`, in texture widths. `Sky::SkyRoll` advances it.
        float mScroll;

        /// Which way each of the two sheets is driven, as a unit bearing in the ground plane.
        ///
        /// **The storm's own direction with its two components swapped, and no angle in between.**
        /// The engine turns each cloud mesh from due north onto that direction, and turning a
        /// crossing by the same angle wants the cosine and the sine of it — which for a unit `(x,
        /// y)` measured from north is `(y, x)`. Reaching that pair through `atan2` and back through
        /// `sin` and `cos` costs three transcendentals a sample and arrives at the same place.
        ///
        /// **One each, because the engine turns each mesh by its own weather's storm.** A
        /// transition into an ashstorm drives the sheet ahead off Red Mountain while the one
        /// overhead still runs due north.
        vec2 mBearing;
        vec2 mNextBearing;

        /// How far the layer falls away over the ground it covers, and the three crossing radii the
        /// engine's own fade turns on. `CloudShell` holds what each of them means and why neither is
        /// a constant.
        float mCurvature;
        vec3 mRings;

        uint mTexture;
        uint mNext;
    };

    /// How many patches the night sky is painted with, over and above the star field.
    RTX_CONST uint SKY_PATCH_COUNT = 6u;

    /// One of them, as a ray that reached nothing finds it.
    ///
    /// **The same thing a moon is**, and drawn the same way: a direction, an angular size, and a
    /// sheet laid across the face. `Sky::nightPatches` says where the six are and how big, measured
    /// off the mesh the rasterizer hangs them on.
    struct SkyPatch
    {
        vec3 mDirection;

        /// The face's own axes, unit and square to `mDirection` and to each other.
        vec3 mRight;
        vec3 mUp;

        /// Half the angle it subtends, in radians. The nebulae reach past a radian, which is why
        /// they read as a tint over the sky rather than as something in it.
        float mAngularRadius;

        uint mTexture;
    };

    /// The star field.
    ///
    /// **Stars are on a sphere and clouds are on a plane**, which is the whole difference between
    /// this and `CloudDeck`: a cloud layer converges at the horizon and a star does not move as the
    /// eye does. The sheet is laid on that sphere at the scale the engine's own mesh lays it at.
    struct StarField
    {
        /// How much of the sheet is there: the engine's `Stars` ramp times the weather's glare, so
        /// stars come out at dusk and an overcast keeps them in.
        float mFade;

        /// What every sheet of the night sky adds to what the sky *lights* with, already faded.
        ///
        /// **The sheets as a source rather than as a picture**, and the two are reached differently:
        /// a ray that is looked along samples them where it points, and one gathering a hemisphere
        /// takes this instead. `NightSky::mGlow` says why a mean and not the sheets themselves.
        vec3 mGlow;

        /// How far the sphere has rolled about the zenith, in radians. Once every four days.
        float mTurn;

        /// How much sky one tile of the sheet covers, in radians — **read off the mesh** rather than
        /// chosen, and it is what decides how big a star is. `Rtx::NightSky` measures it as the
        /// median rate the unwrap runs at, and the unwrap is isotropic, which is what keeps a star
        /// round. Morrowind's comes to about a tenth of a degree per texel; the same sheet spread
        /// once over the hemisphere would be a third, which is a blob.
        float mTile;

        /// The elevation the field fades out below, in radians. The mesh's again: the engine draws a
        /// vertex of that dome only where its authored colour is white, and its bottom ring is not.
        float mHorizon;

        uint mTexture;
    };

    /// One of the two moons, as a disc a ray that reached nothing can find.
    ///
    /// **A disc and not a body**, for the reason the sun is: nothing puts a sphere in an
    /// acceleration structure, so a moon is a direction with a size and a face painted across it.
    /// What that buys is the same thing the sun's disc buys — water traces a reflection ray and
    /// finds the moon in it for nothing, and there is one place a moon's size lives.
    struct MoonDisc
    {
        /// Unit vector toward the moon, and the two axes its face is painted along. The face turns
        /// against the horizon as the moon crosses, which is what a tidally locked moon does and
        /// what a billboard does not.
        vec3 mDirection;
        vec3 mRight;
        vec3 mUp;

        /// What a fully lit face sends back, linear.
        vec3 mColour;

        /// What the moon delivers to a surface facing it, linear.
        ///
        /// **A light and the disc are two numbers here, not one.** `Shaders::MOON_ALBEDO` says why
        /// the level a moon lights by cannot be read off the radiance it is drawn at. Zero is a
        /// moon that lights nothing, and it is the one test worth making before a shadow ray.
        vec3 mIrradiance;

        /// Half the angle the disc subtends. Masser's is between five and a half degrees and nine
        /// and a half, on the two `Moons_Masser_Size` the game ships — twenty to thirty-six times
        /// the sun either way.
        float mAngularRadius;

        /// How far round its cycle: zero is full and pi is new.
        ///
        /// **The share that is lit comes from the game and the direction it faces comes from the
        /// sky.** Morrowind advances a phase on its own three-day clock, which owes nothing to where
        /// its sun actually is — so the terminator is carved at the angle the game names and then
        /// turned so the lit limb points at the sun, which is the only orientation that does not
        /// read as a mistake.
        float mPhaseAngle;

        /// What the game fades the moon by near the horizon and at the ends of its arc. Zero is a
        /// moon that is not there, and the whole disc is skipped for it.
        float mAlpha;

        /// What the air leaves of it, per channel.
        ///
        /// **What a moon is dimmed by on the way up, in place of being switched off.** The engine
        /// draws none under `Moons_<name>_Fade_End_Angle`; here the slant path through the air does
        /// that and does it from the horizon, so a moon comes over the edge as a deep red ember.
        /// `Rtx::airTransmittance` carries the two published figures it is made of.
        ///
        /// **It dims and never uncovers.** What stands behind a moon is hidden by `mAlpha` alone,
        /// because a moon low in the air still blocks a star — what replaces it there is the airlight
        /// in front, which is the dome, and the dome is added over the whole sky anyway.
        vec3 mThroughAir;

        /// The painted face, in the bindless array, or `NO_TEXTURE` where none was loaded — the disc
        /// is then its mean colour with the shading law over it, which is what a moon looked like
        /// before the faces arrived.
        ///
        /// **The `full` portrait and only that one.** The game ships eight per moon and this draws
        /// the terminator itself, so what is wanted from the file is the maria and the silhouette —
        /// one face under eight lightings, which is what a tidally locked moon is.
        ///
        /// **The alpha is not premultiplied.** Past the edge of the painted disc the file's colour
        /// climbs back toward the middle of its range, so a sampler that took the colour and dropped
        /// the alpha drew a bright ring around every moon. Multiplying by it removes that and hands
        /// over the limb's own antialiasing for nothing.
        uint mFace;
    };

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
        /// **The two things an ambient can be, told apart.** Out of doors it is the sky: a point that
        /// cannot see the sky does not get it, which is what `pathEnd` occludes. Inside, it is the
        /// cell's own `AMBI` — a fill standing for every bounce the room makes, which reaches a
        /// corner as much as the middle of the floor — and occluding that by what a ray can escape
        /// to would take the light out of every interior, since none of them escapes anything.
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

        /// How wide each of the sea's tiles is, in world units, in the order they are bound.
        ///
        /// **What turns a world position into a texture coordinate and a cone width into a level.**
        /// The tiles are a compile-time table and could have been a shader constant; they are handed
        /// over instead so what the shader divides by is what `WavePass` actually built, rather than
        /// a second copy of it that a change to the first would leave behind.
        float mWaveExtent[WAVE_CASCADES];

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

        /// Masser and Secunda, in that order. An interface trace and an interior leave both at an
        /// alpha of nothing, which costs the sky one compare each.
        MoonDisc mMoons[2];

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
    };

    // Pinned for the reason `scene.h` gives: the side that writes these bytes and the side that
    // reads them are different compilers.
#if defined(RTX_HOST) || defined(__METAL_VERSION__)
    static_assert(sizeof(MoonDisc) == 88, "MoonDisc must be scalar-packed on every side");
    static_assert(sizeof(CloudDeck) == 96, "CloudDeck must be scalar-packed on every side");
    static_assert(sizeof(StarField) == 32, "StarField must be scalar-packed on every side");
    static_assert(sizeof(SkyPatch) == 44, "SkyPatch must be scalar-packed on every side");
    static_assert(sizeof(VisibilityConstants) == 924, "VisibilityConstants must be scalar-packed on every side");
#endif

#ifdef RTX_HOST
}
#endif

// What both shading languages read and the host does not, for the reason `RTX_SHADER` gives.
#ifndef RTX_HOST

/// The sky's own colour along a direction: the game's horizon fading to its zenith.
///
/// **The two colours rather than the frame they sit in**, because a shared header may not name an
/// address space and Metal's reference to a constant buffer must. What this is about is a gradient
/// between two colours, which is a thing neither backend has an opinion on.
///
/// Morrowind records one colour for the fog and for the sky's lower half because they are the same
/// thing seen at two distances, so a ray that reaches nothing has to converge on exactly what a ray
/// through a mile of air does.
RTX_SHADER vec3 skyGradient(vec3 horizon, vec3 zenith, vec3 direction)
{
    return mix(horizon, zenith, clamp(direction.z, 0.0, 1.0));
}

#endif

#endif
