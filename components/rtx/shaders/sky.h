// `#pragma once` everywhere else in this tree, and an include guard here: `glslc` warns
// "'#pragma once' : not implemented" and carries on, so a header included twice by one
// shader would redefine everything in it.
#ifndef OPENMW_COMPONENTS_RTX_SHADERS_SKY_H
#define OPENMW_COMPONENTS_RTX_SHADERS_SKY_H

#include "hosttypes.h"
#include "portable.h"

// What the game says is over the world: the weather it is, the deck and the sheets that are drawn,
// the discs that are drawn and light, and the gradient behind all of them.
//
// **Split from `visibility.h` because two passes want this and not the frame.** The tone pass draws
// the stars and the fog's set names one layer a source, and each reached the whole frame
// description for one struct — `VisibilityConstants` is eleven hundred bytes, and `tone.comp`
// declared a 64-bit extension it makes no use of to get at `StarField`.

#ifdef RTX_HOST
namespace Rtx::Shaders
{
#endif

    /// Morrowind's ten weathers, in the order `MWWorld::WeatherManager` registers them.
    ///
    /// That order is not an arrangement of this renderer's: it is what a weather's script id counts
    /// along, and it is the order the `Weather_<name>_*` keys sit in a content file. Naming them
    /// here is what lets the game hand over a script id and the harness a name off a command line
    /// and have the two mean one sky — `Rtx::weatherIndex` is the table that joins them.
    const uint WEATHER_CLEAR = 0u;
    const uint WEATHER_CLOUDY = 1u;
    const uint WEATHER_FOGGY = 2u;
    const uint WEATHER_OVERCAST = 3u;
    const uint WEATHER_RAIN = 4u;
    const uint WEATHER_THUNDERSTORM = 5u;
    const uint WEATHER_ASHSTORM = 6u;
    const uint WEATHER_BLIGHT = 7u;
    const uint WEATHER_SNOW = 8u;
    const uint WEATHER_BLIZZARD = 9u;
    const uint WEATHER_COUNT = 10u;

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
    const uint SKY_PATCH_COUNT = 6u;

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

        /// The sine of half the angle it subtends, which is how far off the centre line a
        /// direction at the limb stands. The nebulae reach past a radian, which is why they read as
        /// a tint over the sky rather than as something in it.
        ///
        /// **The sine and not the angle**, for the reason `CloudDeck::mBearing` gives: every reader
        /// wants it through `sin`, and a ray is not the place to take the sine of a number that is
        /// the same for the whole frame. The host has the angle and keeps it.
        float mLimb;

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
    /// One source in the sky as a shading point sees it: the sun, or a moon. What the eye sees of
    /// a disc is `MoonDisc`'s and the sun's own field; this is the half that lights.
    ///
    /// **Three of them and one rule, where there were a sun block and a moon block.** A surface and
    /// a froxel of the air weigh each by what it would deliver unshadowed, draw one, trace to it and
    /// divide by the draw — the lamps' own estimator. In daylight the moons weigh nothing and the sun
    /// is always drawn; at night the sun weighs nothing and the draw is between the moons; and the
    /// hour either side of dusk spends one ray where it spent two, and carries the noise. `skySourceAt`
    /// in `lib/lights.glsl` is where the three are read off the frame.
    struct SkySource
    {
        /// Unit, from a point toward the source.
        vec3 mDirection;

        /// Nought where the source is down or faded out, which is the one test worth making before
        /// a ray.
        vec3 mIrradiance;

        /// The sine of the half angle a shadow ray is drawn across: the sun's `SUN_SHADOW_RADIUS`,
        /// and a moon's own limb.
        float mLimb;
    };

    const uint SKY_SOURCE_SUN = 0u;
    const uint SKY_SOURCE_MASSER = 1u;
    const uint SKY_SOURCE_SECUNDA = 2u;
    const uint SKY_SOURCES = 3u;

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

        /// The sine of half the angle the disc subtends, which is how far off the centre line a
        /// direction at the limb stands. Masser's angle is between five and a half degrees and nine
        /// and a half, on the two `Moons_Masser_Size` the game ships — twenty to thirty-six times
        /// the sun either way.
        ///
        /// **The sine and not the angle**, for the reason `SkyPatch::mLimb` gives.
        float mLimb;

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

    // Pinned for the reason `scene.h` gives: the side that writes these bytes and the side that
    // reads them are different compilers.
#ifdef RTX_HOST
    static_assert(sizeof(MoonDisc) == 88, "MoonDisc must be scalar-packed on every side");
    static_assert(sizeof(CloudDeck) == 96, "CloudDeck must be scalar-packed on every side");
    static_assert(sizeof(StarField) == 32, "StarField must be scalar-packed on every side");
    static_assert(sizeof(SkyPatch) == 44, "SkyPatch must be scalar-packed on every side");
#endif

#ifdef RTX_HOST
}
#endif

// What the shading language reads and the host does not, for the reason `RTX_SHADER` gives.
#ifndef RTX_HOST

/// The sky's own colour along a direction: the game's horizon fading to its zenith.
///
/// **The two colours rather than the frame they sit in.** What this is about is a gradient between
/// two colours, and a function that took the frame would tie itself to how a backend binds one.
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
