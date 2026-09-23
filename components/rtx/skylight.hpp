#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include <osg/Vec3f>

#include <components/esm3/loadcell.hpp>
#include <components/misc/constants.hpp>
#include <components/sky/timeofday.hpp>

#include "fogbuilder.hpp"

namespace Rtx
{
    /// The sun: a direction and no position. Only `makeSkylight` fills it, and a room's light
    /// leaves it at nothing.
    struct Sun
    {
        /// Where the sun stands, unit, so `-mPosition` is where its light travels.
        osg::Vec3f mPosition{ 0.0f, 0.0f, 1.0f };

        /// Irradiance on a surface square to it, linear. Zero exactly when there is no sun, which
        /// is the invariant everything the sun does is gated on.
        osg::Vec3f mIrradiance;

        /// What the disc is painted with, linear: not the hue of `mIrradiance`, which is blue at night.
        osg::Vec3f mDiscColour{ 1.0f, 1.0f, 1.0f };
    };

    /// What a weather says about the sky at one hour, in the renderer's own units, for `makeSkylight`.
    struct SkyReading
    {
        /// Where the disc stands, unit: `MWRender::sunDiscOf`.
        osg::Vec3f mSunPosition = osg::Vec3f(0.0f, 0.0f, 1.0f);

        /// How much of the sun is over the horizon (`sunShareAt`), which alone answers "is there a
        /// sun", and how much of it a layer standing above the ground still has (`sunShareAloft`).
        float mSunShare = 0.0f;
        float mSunShareAloft = 0.0f;

        /// The weather's `Sun_*_Color` and `Ambient_*_Color` at this hour, linear, night blue and all.
        osg::Vec3f mSunColour;
        osg::Vec3f mAmbient;

        /// What the disc is painted with, linear: `MWRender::WeatherResult::mSunDiscColor`.
        osg::Vec3f mDiscColour = osg::Vec3f(1.0f, 1.0f, 1.0f);

        /// The weather's `Glare_View`: how much of the sun it lets through.
        float mGlare = 1.0f;
    };

    /// The sky's light in the two forms a tracer can use it: one that comes from somewhere, and
    /// one that does not.
    struct Skylight
    {
        Sun mSun;

        /// The same sun as a layer above the ground sees it, out of `SkyReading::mSunShareAloft`.
        Sun mSunAloft;

        /// What a path is terminated with: the ambient plus whatever of the sun is not over the horizon.
        osg::Vec3f mAmbient;

        /// What to hold a measured exposure back by for this light; one leaves it alone. A field,
        /// because an interior's ambient is the whole of its light and `exposureBias` would hold a
        /// cellar back by two stops.
        float mExposureBias = 1.0f;
    };

    /// The sky's light out of what a weather says: the one place a sun is built. Morrowind never
    /// switches its sunlight off — `Sun_Night_Color` is `59, 97, 176` — and traced, a night sun
    /// casts hard shadows swinging across the ground. So the night's sun goes into the ambient and
    /// the sun keeps only what is over the horizon; the two are complements, so the total is
    /// continuous through dusk. The ambient's share is `E / 4pi`: a directional source delivers a
    /// quarter of its irradiance averaged over every orientation.
    Skylight makeSkylight(const SkyReading& sky);

    /// What to hold a measured exposure back by for a sky delivering this much light: one where
    /// the hour delivers a full sun's worth or more, falling from there. A histogram normalises
    /// whatever it is shown toward the key, so a midnight and a noon come out alike.
    float exposureBias(const osg::Vec3f& sunIrradiance, const osg::Vec3f& ambient);

    /// How high the cloud layer stands, in world units: the one number in the sky that is chosen
    /// rather than read, because the cloud mesh gives its height in tiles of its own sheet. Five
    /// hundred metres is a stratocumulus base. A world height, so the deck's shadow does not move
    /// with the camera.
    inline constexpr float sCloudAltitude = 500.0f * Constants::UnitsPerMeter;

    /// How much of the sun is over the horizon at `hour`: the weather manager's own disc alpha
    /// (`Sky::sunDiscAlpha`) under its own gate on the night (`Sky::sunUp`), bounded at one. One
    /// rule, lifted, so the disc the rasterizer draws and the shadow the tracer casts come and go
    /// together.
    float sunShareAt(float hour, const Sky::TimeOfDaySettings& times);

    /// How fast the disc's elevation changes near either end of the day, in radians per hour: what
    /// a layer standing above the ground converts its own horizon into, because Morrowind's sunset
    /// is a clock and not an elevation. Constant, because the disc's height is `400 - |east|` and
    /// the swing is linear in the hour: eight degrees an hour over the shipped fourteen-hour day.
    float sunDescentPerHour(const Sky::TimeOfDaySettings& times);

    /// How much of the sun a layer standing over the ground still has at `hour`: `sunShareAt` at
    /// an hour shifted by the dip a layer that high sees, over the time the disc takes to fall it —
    /// 0.718 degrees at 5.35 game minutes on the shipped day, 8.7% of the sun still held when the
    /// ground's goes out. The colour is untouched, because `Sun_Sunset_Color` is keyed on the hour.
    float sunShareAloft(float hour, const Sky::TimeOfDaySettings& times);

    /// What the sky delivers to a surface facing it, and how much of that it is never drawn with.
    struct SkyBudget
    {
        /// The whole of it, as a radiance: the gradient, the night's sheets and `mFill` together.
        osg::Vec3f mMean;

        /// What the sky delivers as light over and above the colour it is drawn with. The engine
        /// puts `Ambient_<weather>_Night_Color` on every surface, while a bounce ray at the dome
        /// reads `Sky_<weather>_Night_Color`, a tenth of it; a gradient linear in `sin(elevation)`
        /// delivers `horizon / 3 + 2 * zenith / 3`, the sheets add their mean, and whatever the
        /// ambient asks for beyond the two is this. Nought by day.
        osg::Vec3f mFill;
    };

    /// Reads both off one weather, so nothing can hold two ideas of what a sky is worth.
    /// `sheets` is what the night sky's own layers add: `Shaders::StarField::mGlow`.
    SkyBudget skyBudget(
        const osg::Vec3f& horizon, const osg::Vec3f& zenith, const osg::Vec3f& sheets, const osg::Vec3f& ambient);

    /// The sun, the sky and the air of one cell, in the renderer's own units, so nothing downstream
    /// reads a content file again.
    struct Daylight
    {
        Skylight mLight;

        /// Sky radiance, linear, at the horizon and overhead. The horizon is the weather's fog
        /// colour, which is also the air the cloud deck hangs in.
        osg::Vec3f mSkyHorizon;
        osg::Vec3f mSkyZenith;

        /// How far the stars have come out: the engine's `Stars` ramp at this hour, before the
        /// weather's glare is taken off it.
        float mStarFade = 0.0f;

        /// The weather's own air, coloured `mSkyHorizon`: Morrowind records one colour for the fog
        /// and the sky's lower half, so a ray that reaches nothing and a ray through a mile of air
        /// arrive at the same answer.
        Fog mFog;
    };

    /// A weather's index, as `MWWorld::WeatherManager` registers them, or nothing for a name that is
    /// none of the ten.
    std::optional<std::uint32_t> weatherIndex(std::string_view weather);

    /// The name that index spells. Empty for an index past the ten.
    std::string_view weatherName(std::uint32_t weather);

    /// A room's light out of its own `AMBI` record, and not the rasterizer's reading of it, which
    /// lifts the ambient to `minimum interior brightness` for a falloff curve this renderer has not
    /// got and aims the sunlight along `(-1, 45°, 45°)` — traced, hard shadows off nothing through
    /// every crack a room's shell is built from. So the sunlight is kept whole and its direction
    /// taken away, over `INV_FOUR_PI`. The sky is the fog colour at both ends and never a light.
    /// `nightEye` is what the Night-Eye effect adds to the ambient, in the file's own space.
    Daylight makeRoomLight(const ESM::Cell::AMBIstruct& room, const osg::Vec3f& nightEye = osg::Vec3f());

    /// What the air leaves of a body in the sky, per channel: Rayleigh optical depth at the three
    /// sRGB primaries — `0.008569 λ^-4` with its usual correction, 0.068, 0.097 and 0.221 at the
    /// zenith — times Kasten and Young's air mass along the slant path, 37.92 at the horizon. A
    /// moon comes up a deep red ember and is itself by thirty degrees; the sun's sunset is the
    /// content's own. `upward` is the `z` of a unit direction, and at or below nothing gives the
    /// horizon's figure.
    osg::Vec3f airTransmittance(float upward);
}
