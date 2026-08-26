#include "lightbuilder.hpp"

#include "distantland.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

#include <components/esm3/loadligh.hpp>
#include <components/esm3/loadregn.hpp>
#include <components/fallback/fallback.hpp>
#include <components/sky/sun.hpp>
#include <components/sky/timeofday.hpp>
#include <components/weather/downpour.hpp>

#include "shaders/scene.h"
#include "shaders/visibility.h"

namespace Rtx
{
    namespace
    {
        /// How bright a light is at half its recorded radius.
        ///
        /// There is no value in the record to be faithful to, so this is the whole of the scale and
        /// it was set by eye. Provisional in a specific way: vanilla textures have light painted
        /// into them already, so every lamp here is competing with illumination that is in the
        /// albedo, and this number only starts to mean something once that is unpicked.
        ///
        /// The pi is the Lambertian `1/pi` the shader divides by, and cancels against it exactly —
        /// so a lamp is measured on the scale below and the sun, which carries no such factor, is
        /// measured a pi apart from it. Both sides read the one constant.
        const float sIntensity = 0.25f * Shaders::PI;

        /// How much further a light reaches than its record says, and how much further again.
        ///
        /// Morrowind's radii run 64 to 256 units in an interior — a metre to three and a half at
        /// seventy units to the metre — so a lantern lights its own post and nothing else. That was
        /// a fixed falloff curve in a renderer with no bounce, where an ambient term filled the
        /// room; here the ambient is real light and the lamps have to be what lights the place.
        ///
        /// Scaling alone widens the gap it is meant to close: a candle's 64 units doubles to 128,
        /// which is still nothing, while a lantern's 256 gains a whole lantern's worth. The flat
        /// term narrows the two instead, and it is the candles that most need to leave their table.
        constexpr float sReachScale = 2.0f;
        constexpr float sReachBonus = 128.0f;

        /// How much of a lamp's recorded radius is actually alight, which is what its shadows are
        /// soft by.
        ///
        /// **The record says how far a lamp reaches and not how big it is** — but the two are
        /// already tied here, and by the line above rather than by this one. The intensity scales
        /// with the *square* of the recorded radius, which is the law for an emitter of fixed
        /// radiance whose area grows with its size; reading that same radius as a size again is
        /// that statement kept rather than a second one made, so a candle and a brazier come to
        /// differ in how soft their shadows are for the reason they already differ in how bright
        /// they are.
        ///
        /// A sixteenth across Morrowind's interior range of 64 to 256 units is a source four to
        /// sixteen units in radius — eleven to forty-six centimetres across at seventy units to the
        /// metre, which is a candle flame and a brazier's bowl. Wider and a lamp dissolves its own
        /// fixture into the shadow it casts; narrower and it is a point again.
        constexpr float sSourceFraction = 1.0f / 16.0f;

        /// Morrowind's ten weathers, in `MWWorld::WeatherManager`'s registration order — which is
        /// what a script id counts along and what a `Weather_<name>_*` key spells. The shader names
        /// the same order as `WEATHER_*`; this is the only place the spellings live.
        constexpr std::array<std::string_view, Shaders::WEATHER_COUNT> sWeathers = {
            "Clear",
            "Cloudy",
            "Foggy",
            "Overcast",
            "Rain",
            "Thunderstorm",
            "Ashstorm",
            "Blight",
            "Snow",
            "Blizzard",
        };

        float channelToLinear(float encoded)
        {
            return encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
        }

    }

    namespace
    {
        /// One weather's numbers at one hour, before any of them is converted.
        ///
        /// **Kept in the units the file records them in**, because a transition lerps *these* and not
        /// what they become: the game blends the fog's recorded depth and converts once
        /// (`apps/openmw/mwworld/weather.cpp:1090`), and blending two extinctions instead is a
        /// different curve.
        struct Reading
        {
            osg::Vec4f mHaze;
            osg::Vec4f mSky;
            osg::Vec4f mAmbient;
            osg::Vec4f mSun;

            /// The disc's own colour, in the space the file records it in. Built here rather than
            /// in `settle` because the formula reads the ambient in that same space, and because a
            /// transition blends what each weather's disc came to rather than the numbers behind it
            /// — which is what `calculateTransitionResult` does. How much of the disc there is is
            /// not a weather's business and is `Sky::sunShareAt`.
            osg::Vec3f mSunDisc;

            /// How much of the sun a weather lets through, which dims the disc under an overcast.
            float mGlare = 1.0f;

            float mFogDepth = 0.0f;
        };

        Reading readWeather(std::string_view weather, const Sky::TimeOfDaySettings& times, float hour)
        {
            const std::string name(weather);

            // **The game's own four-point ramp rather than a step between four phases.** Each
            // quantity crosses dawn over a window of its own — the sun can be up before the sky has
            // finished turning — so reading whichever phase an hour fell in got every hour inside a
            // transition wrong, which is most of sunrise and most of dusk.
            const auto ramp = [&name, &times, hour](std::string_view field) {
                const std::string prefix(field);
                const auto colour = [&name, &prefix](std::string_view phase) {
                    return Fallback::Map::getColour(
                        "Weather_" + name + "_" + prefix + "_" + std::string(phase) + "_Color");
                };

                return Sky::TimeOfDayInterpolator<osg::Vec4f>(
                    colour("Sunrise"), colour("Day"), colour("Sunset"), colour("Night"))
                    .getValue(hour, times, prefix);
            };

            // **A content file records a day depth and a night one and nothing between**, so the
            // game hands the day value to three of the ramp's four points and lets it cross to night
            // at dusk. Asking for a sunrise depth is not a key that reads zero — the fallback map
            // does not know it at all and throws.
            const float day = Fallback::Map::getFloat("Weather_" + name + "_Land_Fog_Day_Depth");
            const float night = Fallback::Map::getFloat("Weather_" + name + "_Land_Fog_Night_Depth");

            const osg::Vec4f ambient = ramp("Ambient");

            return Reading{
                .mHaze = ramp("Fog"),
                .mSky = ramp("Sky"),
                .mAmbient = ambient,
                .mSun = ramp("Sun"),
                .mSunDisc = Sky::sunDiscAt(
                    hour, times, Fallback::Map::getColour("Weather_" + name + "_Sun_Disc_Sunset_Color"), ambient),
                .mGlare = Fallback::Map::getFloat("Weather_" + name + "_Glare_View"),
                .mFogDepth = Sky::TimeOfDayInterpolator<float>(day, day, day, night).getValue(hour, times, "Fog"),
            };
        }

        Daylight settle(const Reading& read, const Sky::TimeOfDaySettings& times, float hour)
        {
            const Sky::SunPlacement sun = Sky::sunAt(hour, times);
            const osg::Vec3f haze = decodeColour(read.mHaze);

            // **The sun is not assembled here**, and the game does not assemble one either: both
            // hand what their weather says to the one builder that knows what a sun may be.
            const Skylight sky = makeSkylight(SkyReading{
                .mSunPosition = sun.mPosition,
                .mSunShare = sun.mShare,
                .mSunColour = decodeColour(read.mSun),
                .mAmbient = decodeColour(read.mAmbient),
                .mDiscColour = decodeColour(read.mSunDisc),
                .mGlare = read.mGlare,
            });

            return Daylight{
                .mSun = sky.mSun,
                .mSkyHorizon = haze,
                .mSkyZenith = decodeColour(read.mSky),
                .mAmbient = sky.mAmbient,
                .mHaze = read.mHaze,

                // **The engine's own ramp for the stars**, which is four points like every other and
                // crosses on the `Stars` window rather than the sky's: they outlast the sunset and
                // are gone before the sun is up. Nothing but night has any of it.
                .mStarFade = Sky::TimeOfDayInterpolator<float>(0.0f, 0.0f, 0.0f, 1.0f).getValue(hour, times, "Stars"),
                .mFog = { .mColour = haze, .mExtinction = fogExtinction(read.mFogDepth, distantLandReach()) },
            };
        }
    }

    Skylight makeSkylight(const SkyReading& sky)
    {
        // A quarter of the irradiance over pi: what a directional source comes to once its direction
        // is taken away. The header carries the derivation and the reason.
        const float fill = 0.25f * Shaders::INV_PI;

        const osg::Vec3f irradiance = sky.mSunColour * Shaders::DAYLIGHT;
        const float share = std::clamp(sky.mSunShare, 0.0f, 1.0f);

        return Skylight{
            .mSun = { .mPosition = sky.mSunPosition,
                .mIrradiance = irradiance * share,

                // **The glare arrives here rather than being folded into the colour earlier**, and
                // that is not tidiness: it is a blend factor the rasterizer applies to a sprite in
                // the file's own space, and dimming radiance is a linear multiply. Applied before
                // the decode it would come out a different colour, not merely a darker one.
                .mDiscColour = sky.mDiscColour * sky.mGlare },
            .mAmbient = sky.mAmbient + irradiance * ((1.0f - share) * fill),
        };
    }

    Daylight makeDaylight(std::string_view weather, float hour)
    {
        const Sky::TimeOfDaySettings times = Sky::TimeOfDaySettings::fromFallback();
        return settle(readWeather(weather, times, hour), times, hour);
    }

    Daylight makeDaylight(std::string_view from, std::string_view to, float blend, float hour)
    {
        const Sky::TimeOfDaySettings times = Sky::TimeOfDaySettings::fromFallback();
        const Reading a = readWeather(from, times, hour);
        const Reading b = readWeather(to, times, hour);

        const auto mix = [blend](const auto& x, const auto& y) { return x * (1.0f - blend) + y * blend; };

        // Exactly the quantities `calculateTransitionResult` blends, and the depth among them rather
        // than the extinction it becomes.
        return settle(
            Reading{
                .mHaze = mix(a.mHaze, b.mHaze),
                .mSky = mix(a.mSky, b.mSky),
                .mAmbient = mix(a.mAmbient, b.mAmbient),
                .mSun = mix(a.mSun, b.mSun),
                .mSunDisc = mix(a.mSunDisc, b.mSunDisc),
                .mGlare = mix(a.mGlare, b.mGlare),
                .mFogDepth = mix(a.mFogDepth, b.mFogDepth),
            },
            times, hour);
    }

    std::optional<std::uint32_t> weatherIndex(std::string_view weather)
    {
        const auto found = std::find(sWeathers.begin(), sWeathers.end(), weather);
        if (found == sWeathers.end())
            return std::nullopt;

        return static_cast<std::uint32_t>(found - sWeathers.begin());
    }

    float glareView(std::string_view weather)
    {
        return Fallback::Map::getFloat("Weather_" + std::string(weather) + "_Glare_View");
    }

    std::uint32_t nextRegionWeather(const ESM::Region* region, std::uint32_t weather, bool forward)
    {
        const std::uint32_t count = static_cast<std::uint32_t>(sWeathers.size());
        const std::uint32_t step = forward ? 1u : count - 1u;

        // Round once and no further: a region with nothing to offer hands back a step of the plain
        // order rather than spinning, which is what a record of all zeroes would otherwise do.
        std::uint32_t at = (weather + step) % count;
        for (std::uint32_t tried = 0; tried < count; ++tried)
        {
            if (region == nullptr || region->mData.mProbabilities[at] > 0)
                return at;

            at = (at + step) % count;
        }

        return (weather + step) % count;
    }

    std::string_view weatherName(std::uint32_t weather)
    {
        return weather < sWeathers.size() ? sWeathers[weather] : std::string_view();
    }

    osg::Vec3f stormDirection(std::uint32_t weather, const osg::Vec3f& observer)
    {
        // **The rule is `Weather::stormDirection` and the index is this function's own.** The game
        // asks it during a transition and so holds the effect rather than the name; everything here
        // holds a script id, and the two must not be two rules — they aim the same storm at the
        // same observer, one for the sky and one for the particles blowing past it.
        return Weather::stormDirection(Weather::stormEffect(weatherName(weather)), observer);
    }

    osg::Vec3f decodeColour(const osg::Vec4f& encoded)
    {
        return osg::Vec3f(channelToLinear(encoded.x()), channelToLinear(encoded.y()), channelToLinear(encoded.z()));
    }

    osg::Vec3f decodeColour(const osg::Vec3f& encoded)
    {
        return decodeColour(osg::Vec4f(encoded, 1.0f));
    }

    osg::Vec3f decodeColour(std::uint32_t packed)
    {
        const auto channel
            = [](std::uint32_t bits) { return channelToLinear(static_cast<float>(bits & 0xFFu) / 255.0f); };

        return osg::Vec3f(channel(packed), channel(packed >> 8), channel(packed >> 16));
    }

    std::optional<Light> makeLight(const osg::Vec3f& colour, float radius, const osg::Vec3f& position)
    {
        // The radius comes off a file something else wrote, or off a graph something else built, so
        // a nonsensical one is data rather than a broken contract: a light with no size lights
        // nothing and is dropped.
        if (!(radius > 0.0f))
            return std::nullopt;

        return Light{
            .mPosition = position,
            .mIntensity = colour * (radius * radius * sIntensity),
            .mReach = radius * sReachScale + sReachBonus,
            .mRadius = radius * sSourceFraction,
        };
    }

    std::optional<Light> makeLight(const ESM::Light& record, const osg::Vec3f& position)
    {
        constexpr int notPlaced = ESM::Light::Carry | ESM::Light::Negative | ESM::Light::OffDefault;
        if ((record.mData.mFlags & notPlaced) != 0)
            return std::nullopt;

        return makeLight(decodeColour(record.mData.mColor), static_cast<float>(record.mData.mRadius), position);
    }
}
