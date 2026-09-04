#include "lightbuilder.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

#include <components/esm3/loadcell.hpp>
#include <components/esm3/loadregn.hpp>
#include <components/fallback/fallback.hpp>
#include <components/misc/constants.hpp>
#include <components/sceneutil/lightcommon.hpp>
#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/lightutil.hpp>
#include <components/sceneutil/util.hpp>
#include <components/sceneutil/vismask.hpp>
#include <components/sky/clouds.hpp>
#include <components/sky/sun.hpp>
#include <components/sky/timeofday.hpp>
#include <components/weather/downpour.hpp>

#include "error.hpp"
#include "shaders/colour.h"
#include "shaders/look.h"
#include "shaders/scene.h"
#include "shaders/visibility.h"
#include "srgb.hpp"

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

        /// How much of that same radius the fitting around the flame is assumed to fill.
        ///
        /// **A lamp is never bare.** Morrowind's lights hang inside lanterns, sit in sconces and
        /// stand in holders, and the shadow ray this buys has to leave from somewhere on the flame
        /// without ending inside the cage around it. Aimed across the flame and stopped at the flame,
        /// half the rays a wall sends end among that fitting and come back fully shadowed — which
        /// took the whole lamp off those pixels and drew a black speckle over every lamp-lit surface
        /// in the game.
        ///
        /// Measured on the lantern above Seyda Neen's docks and in Balmora's mages guild, as the
        /// share of a lit wall that reads far darker than its neighbours: one flame of clearance
        /// leaves 6.6 % and 4.2 %, two leave 2.6 % and 2.4 %, three leave 1.5 % and 2.1 %, and four
        /// leave 1.2 % and 1.8 % — which is where the black speckle is gone and only the ordinary
        /// grain of one sample a pixel is left. The guild is brighter at every step, because what
        /// the clearance stops charging the lamp for is its own fitting.
        ///
        /// **What it costs is stated rather than hidden**: a real occluder standing closer than a
        /// quarter of a lamp's recorded radius stops casting a shadow from it. That is sixteen units
        /// for a candle and sixty-four for the largest interior lamp, and inside that distance the
        /// fitting is what an occluder nearly always is.
        constexpr float sFittingFraction = 0.25f;

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

            /// What the content files record this weather blowing at, which stands its fog layer up.
            float mWindSpeed = 0.0f;
        };

        /// How much of the sun a weather lets through — `Weather_<name>_Glare_View`, which dims a
        /// sun disc under an overcast and keeps the stars in behind one.
        float glareView(std::string_view weather)
        {
            return Fallback::Map::getFloat("Weather_" + std::string(weather) + "_Glare_View");
        }

        /// One weather's numbers at `hour`, out of a record already read.
        ///
        /// **The game's own four-point ramp rather than a step between four phases.** Each quantity
        /// crosses dawn over a window of its own — the sun can be up before the sky has finished
        /// turning — so reading whichever phase an hour fell in got every hour inside a transition
        /// wrong, which is most of sunrise and most of dusk.
        Reading readHour(const WeatherRamps& ramps, const Sky::TimeOfDaySettings& times, float hour)
        {
            const osg::Vec4f ambient = ramps.mAmbient.getValue(hour, times, "Ambient");

            return Reading{
                .mHaze = ramps.mHaze.getValue(hour, times, "Fog"),
                .mSky = ramps.mSky.getValue(hour, times, "Sky"),
                .mAmbient = ambient,
                .mSun = ramps.mSun.getValue(hour, times, "Sun"),
                .mSunDisc = Sky::sunDiscAt(hour, times, ramps.mDiscSunset, ambient),
                .mGlare = ramps.mGlare,
                .mFogDepth = ramps.mFogDepth.getValue(hour, times, "Fog"),
                .mWindSpeed = ramps.mWindSpeed,
            };
        }

        Daylight settle(const Reading& read, const Sky::TimeOfDaySettings& times, float hour, float reach)
        {
            const Sky::SunPlacement sun = Sky::sunAt(hour, times);
            const osg::Vec3f haze = decodeColour(read.mHaze);
            const osg::Vec3f zenith = decodeColour(read.mSky);

            // **The sun is not assembled here**, and the game does not assemble one either: both
            // hand what their weather says to the one builder that knows what a sun may be.
            const Skylight sky = makeSkylight(SkyReading{
                .mSunPosition = sun.mPosition,
                .mSunShare = sun.mShare,
                .mSunShareAloft = sunShareAloft(hour, times),
                .mSunColour = decodeColour(read.mSun),
                .mAmbient = decodeColour(read.mAmbient),
                .mDiscColour = decodeColour(read.mSunDisc),
                .mGlare = read.mGlare,
            });

            return Daylight{
                .mSun = sky.mSun,
                .mSunAloft = sky.mSunAloft,
                .mSkyHorizon = haze,
                .mSkyZenith = zenith,
                .mAmbient = sky.mAmbient,

                // **The engine's own ramp for the stars**, which is four points like every other and
                // crosses on the `Stars` window rather than the sky's: they outlast the sunset and
                // are gone before the sun is up. Nothing but night has any of it.
                .mStarFade = Sky::TimeOfDayInterpolator<float>(0.0f, 0.0f, 0.0f, 1.0f).getValue(hour, times, "Stars"),
                .mExposureBias = exposureBias(sky.mSun.mIrradiance, sky.mAmbient),
                .mFog = exteriorFog(haze, read.mFogDepth, read.mWindSpeed, reach),
            };
        }
    }

    namespace
    {
        /// How much of the hour's own darkness the exposure keeps, as a power of the light it gives.
        ///
        /// **Two stops and four fifths between a clear noon and a clear midnight**: the two are 8.90
        /// stops apart in what they deliver — 8.03 against 0.0168, by luminance — and `0.314` of
        /// that is 2.79.
        ///
        /// Under three rather than the four and a half the literature fits to real scenes, because
        /// there is no absolute luminance scale here to hang a published curve on: noon to midnight
        /// is five hundred to one in this renderer where the world's is a hundred million to one.
        /// What buys the rest of a night is a different lever from exposure — a Purkinje shift
        /// raises what is dark while it desaturates it, where every exposure lever moves the whole
        /// frame at once.
        constexpr float sHourStops = 0.314f;

        /// Rayleigh optical depth at the zenith, at the three sRGB primaries.
        ///
        /// `0.008569 λ^-4` with its usual correction, at 600, 550 and 450 nanometres — which is near
        /// enough to where the primaries sit. **Aerosol is left out**: how thick the haze is belongs
        /// to a weather rather than to the air, and a number for it here would be one nobody
        /// measured.
        const osg::Vec3f sAirDepth(0.0683f, 0.0973f, 0.2213f);

        /// How far the world curves under that layer — the Earth's own radius, in world units.
        ///
        /// **Not `CloudShell::mCurvature`, which is a shape fit and not a planet.** That is `k · h`
        /// off Morrowind's cap and comes to 0.0575; read as `h / R` it is a world 128 times too
        /// small, and the dip below would come out at 27 degrees rather than a fraction of one.
        const float sWorldRadius = 6371000.0f * Constants::UnitsPerMeter;
    }

    float exposureBias(const osg::Vec3f& sunIrradiance, const osg::Vec3f& ambient)
    {
        const float level = (sunIrradiance + ambient) * Shaders::LUMINANCE_WEIGHTS;

        // **Measured against a full sun rather than against a noon worked out here.** A clear noon
        // delivers 8.03 where `DAYLIGHT` is 8, so the hour that needs no holding back is the one
        // that comes out at one — and no second number has to be kept in step with the first.
        return std::pow(std::min(level / Shaders::DAYLIGHT, 1.0f), sHourStops);
    }

    osg::Vec3f airTransmittance(float upward)
    {
        // Already the sine of the elevation, which is what makes the whole of a unit direction's `z`
        // worth carrying: the fit below wants that and the angle, and only the angle costs a trig
        // call.
        const float sine = std::clamp(upward, 0.0f, 1.0f);
        const float elevation = osg::RadiansToDegrees(std::asin(sine));

        // Kasten and Young's fit, which holds to the horizon where `1 / sin` runs away: 37.92 air
        // masses there against one overhead.
        const float mass = 1.0f / (sine + 0.50572f * std::pow(elevation + 6.07995f, -1.6364f));

        return osg::Vec3f(
            std::exp(-sAirDepth.x() * mass), std::exp(-sAirDepth.y() * mass), std::exp(-sAirDepth.z() * mass));
    }

    SkyBudget skyBudget(
        const osg::Vec3f& horizon, const osg::Vec3f& zenith, const osg::Vec3f& sheets, const osg::Vec3f& ambient)
    {
        // What a uniform sky would have to be to deliver what this gradient does. `skyGradient` runs
        // linearly in `sin(elevation)`, so the cosine-weighted integral over the hemisphere comes to
        // `pi * (horizon / 3 + 2 * zenith / 3)` — two thirds of the sky an up-facing surface sees is
        // nearer the zenith than the horizon, and this is that in closed form. The sheets are already
        // a mean over the hemisphere and need no such weighting.
        const osg::Vec3f carried = horizon / 3.0f + zenith * (2.0f / 3.0f) + sheets;

        const osg::Vec3f fill(std::max(ambient.x() - carried.x(), 0.0f), std::max(ambient.y() - carried.y(), 0.0f),
            std::max(ambient.z() - carried.z(), 0.0f));

        return SkyBudget{ .mMean = carried + fill, .mFill = fill };
    }

    Skylight makeSkylight(const SkyReading& sky)
    {
        const osg::Vec3f irradiance = sky.mSunColour * Shaders::DAYLIGHT;
        const float share = std::clamp(sky.mSunShare, 0.0f, 1.0f);

        // **The share taken this way is a dusk's, because that is the only hour a sun has light to
        // spread and no direction to spread it from.** It is nothing at noon, where the direct term carries all of
        // it, and nothing at night, where there is no sun to take a direction away from — peaking
        // where the disc straddles the horizon and the sky in front of it is the brightest thing in
        // the frame. `2 * s * (1 - s)` is that, and the two leaves a dusk where it was: the ramp
        // this replaces came to a half at the half-set point, and so does this.
        //
        // **The shape that suggests itself is `1 - share`, and it is largest where there is no sun.**
        // Morrowind leaves a blue in the sun's slot all night — `Sun_Night_Color`, which is the
        // original engine's stand-in for moonlight — and spreading that as an ambient came to six
        // times the night ambient the weather itself records, flat, with no direction and no shadow
        // in it. This renderer traces the moons, so keeping it is the moon counted twice and a night
        // that does not read as one.
        const float dusk = 2.0f * share * (1.0f - share);

        return Skylight{
            .mSun = sunAbove(sky, share),
            .mSunAloft = sunAbove(sky, sky.mSunShareAloft),
            .mAmbient = sky.mAmbient + irradiance * (dusk * Shaders::INV_FOUR_PI),
        };
    }

    Sun sunAbove(const SkyReading& sky, float share)
    {
        return Sun{
            .mPosition = sky.mSunPosition,
            .mIrradiance = sky.mSunColour * (Shaders::DAYLIGHT * std::clamp(share, 0.0f, 1.0f)),

            // **The glare arrives here rather than being folded into the colour earlier**, and that
            // is not tidiness: it is a blend factor the rasterizer applies to a sprite in the file's
            // own space, and dimming radiance is a linear multiply. Applied before the decode it
            // would come out a different colour, not merely a darker one.
            .mDiscColour = sky.mDiscColour * sky.mGlare,
        };
    }

    float sunShareAloft(float hour, const Sky::TimeOfDaySettings& times)
    {
        // How far under the ground's horizon a layer that high still sees the sun, and how long the
        // disc takes to fall that far.
        const float dip = std::sqrt(2.0f * sCloudAltitude / sWorldRadius);
        const float descent = Sky::sunDescentPerHour(times);
        if (!(descent > 0.0f))
            return Sky::sunShareAt(hour, times);

        // **The larger of the ramp read either side, because the layer's day is the ground's widened
        // at both ends.** A layer that sees the sun lower sees it earlier in the morning and later
        // in the evening, and those are opposite shifts of one clock — reading an earlier hour is
        // right at dusk and hands the morning less sun than the ground itself gets.
        const float offset = dip / descent;

        return std::max(Sky::sunShareAt(hour - offset, times), Sky::sunShareAt(hour + offset, times));
    }

    WeatherRamps readWeatherRamps(std::string_view weather)
    {
        // A name that is none of the ten is left to the map, which refuses it as a key it will
        // not consider; one of the ten with nothing written for it is refused here, by name.
        if (weatherIndex(weather).has_value())
            requireWeather(weather, Fallback::Map::getFloatFallbackMap(), Fallback::Map::getNonNumericFallbackMap());

        // `Sky::colourRamp` spells the four keys, and `MWWorld::Weather` builds its own out of the
        // same call.
        return WeatherRamps{
            .mHaze = Sky::colourRamp(weather, "Fog"),
            .mSky = Sky::colourRamp(weather, "Sky"),
            .mAmbient = Sky::colourRamp(weather, "Ambient"),
            .mSun = Sky::colourRamp(weather, "Sun"),
            .mFogDepth = Sky::landFogRamp(weather),
            .mDiscSunset = Fallback::Map::getColour("Weather_" + std::string(weather) + "_Sun_Disc_Sunset_Color"),
            .mGlare = glareView(weather),

            // **The recorded speed and not the gust.** What this decides is how deep the layer
            // stands and how fast the field is carried, both of which are the weather's settled
            // character rather than the number the engine wanders about it.
            .mWindSpeed = Weather::windSpeed(weather),

            .mCloudSpeed = Sky::cloudSpeed(weather),
            .mCloudsMaximumPercent = Sky::cloudsMaximumPercent(weather),
        };
    }

    Daylight makeDaylight(const WeatherRamps& ramps, float hour, float reach)
    {
        const Sky::TimeOfDaySettings& times = Sky::TimeOfDaySettings::shared();
        return settle(readHour(ramps, times, hour), times, hour, reach);
    }

    Daylight makeDaylight(std::string_view weather, float hour, float reach)
    {
        return makeDaylight(readWeatherRamps(weather), hour, reach);
    }

    Daylight makeDaylight(const WeatherRamps& from, const WeatherRamps& to, float blend, float hour, float reach)
    {
        const Sky::TimeOfDaySettings& times = Sky::TimeOfDaySettings::shared();
        const Reading a = readHour(from, times, hour);
        const Reading b = readHour(to, times, hour);

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
                .mWindSpeed = mix(a.mWindSpeed, b.mWindSpeed),
            },
            times, hour, reach);
    }

    void requireWeather(std::string_view weather, const std::map<std::string, float, std::less<>>& floats,
        const std::map<std::string, std::string, std::less<>>& strings)
    {
        // What `readWeather`, the clouds and the wind read of a weather, and nothing a script or the
        // sound engine does: the keys whose nought would be a picture rather than a silence.
        constexpr std::array<std::string_view, 4> rampNames = { "Sky", "Fog", "Ambient", "Sun" };
        constexpr std::array<std::string_view, 4> phases = { "Sunrise", "Day", "Sunset", "Night" };
        constexpr std::array<std::string_view, 2> colours = { "Sun_Disc_Sunset_Color", "Cloud_Texture" };
        constexpr std::array<std::string_view, 6> numbers = { "Land_Fog_Day_Depth", "Land_Fog_Night_Depth",
            "Glare_View", "Wind_Speed", "Cloud_Speed", "Clouds_Maximum_Percent" };

        const std::string prefix = "Weather_" + std::string(weather) + "_";
        const auto refuse = [&weather](const std::string& key) {
            throw Error("weather \"" + std::string(weather) + "\" has no \"" + key
                + "\" in openmw.cfg: run openmw-iniimporter on Morrowind.ini, which writes every weather's keys");
        };

        for (const std::string_view ramp : rampNames)
            for (const std::string_view phase : phases)
            {
                const std::string key = prefix + std::string(ramp) + "_" + std::string(phase) + "_Color";
                if (!strings.contains(key))
                    refuse(key);
            }

        for (const std::string_view colour : colours)
            if (const std::string key = prefix + std::string(colour); !strings.contains(key))
                refuse(key);

        for (const std::string_view number : numbers)
            if (const std::string key = prefix + std::string(number); !floats.contains(key))
                refuse(key);
    }

    std::optional<std::uint32_t> weatherIndex(std::string_view weather)

    {
        const auto found = std::find(sWeathers.begin(), sWeathers.end(), weather);
        if (found == sWeathers.end())
            return std::nullopt;

        return static_cast<std::uint32_t>(found - sWeathers.begin());
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
        return toLinear(osg::Vec3f(encoded.x(), encoded.y(), encoded.z()));
    }

    osg::Vec3f decodeColour(const osg::Vec3f& encoded)
    {
        return decodeColour(osg::Vec4f(encoded, 1.0f));
    }

    /// The top of the ladder every animation is built from, in hertz.
    ///
    /// **A flame's puffing frequency, capped by what one sample a frame can carry.** A buoyant
    /// diffusion flame sheds a vortex ring at about `1.5 / sqrt(D)` hertz, with `D` its width in
    /// metres, so a lamp flame near 28 mm across puffs at nine. Nine is also as high as this can
    /// usefully reach: the light is read once a frame, which is 6.7 samples a cycle at 60 frames a
    /// second and 3.3 at 30. Anything faster reads as noise at the first rate and aliases into a
    /// slower beat at the second.
    constexpr float sTopBand = 9.0f;

    /// One step down the ladder of bands, and the step between a fast animation and its slow twin.
    ///
    /// **The golden ratio squared, because it is irrational.** Bands at a rational ratio come back
    /// into phase and the whole flicker repeats on that period, which a viewer standing still in a
    /// lit room sees. These never do.
    constexpr float sBandRatio = 2.618034f;

    /// How many bands a flame is the sum of. Four spans a factor of eighteen in rate, which is the
    /// whole of what a flame does: the puffing at the top, and a draught wandering under it.
    constexpr int sFlameBands = 4;

    /// How far a flame swings, as a fraction of what the light radiates.
    ///
    /// This is the peak, and the bands are weighted to sum to one, so the brightness lands in
    /// `1 +- sFlameDepth` exactly. What it usually is is far smaller: with equal bands the deviation
    /// is `sFlameDepth / sqrt(2 * sFlameBands)` RMS, which is 11% of the light. A candle burning in
    /// still air varies by about a tenth of its output, and a peak three times that is the draught.
    constexpr float sFlameDepth = 0.30f;

    /// How far a pulse swings. Deeper than a flame, because a pulse is the whole of what the light
    /// does: the content gives it to lava, to glowing lichen, to Dwemer tubes and to enchanted
    /// rings, and none of those has a flame for it to be a variation of.
    constexpr float sPulseDepth = 0.35f;

    /// The slow pulse, in hertz. Three seconds a cycle reads as a swell rather than as a flicker,
    /// which is the whole difference between the two kinds.
    constexpr float sPulseBand = 1.0f / 3.0f;

    /// How far apart one light's bands are set, in turns. The golden ratio's conjugate spreads any
    /// number of them around the circle without two landing together.
    constexpr float sBandPhase = 0.618034f;

    /// Where a light stands in its animation, in turns.
    ///
    /// **Off the light's id, so that it is drawn once and never kept.** Two candles standing
    /// together must not swing as one, and a phase rolled at random would put the harness's lamp
    /// somewhere else on every run. Ids are handed out in sequence, so they are multiplied by an odd
    /// constant near the golden ratio's share of the word to scatter neighbours.
    float lightPhase(int id)
    {
        const std::uint32_t scattered = static_cast<std::uint32_t>(id) * 2654435761u;
        return static_cast<float>(scattered >> 8) * 0x1p-24f;
    }

    /// One sine of the ladder: `index` steps this light's phase along, `frequency` is in hertz.
    float band(double simulationTime, float frequency, float phase, int index)
    {
        // **Reduced to one turn in double, before it is narrowed.** A session's clock reaches tens
        // of thousands of seconds, and a float holding that many turns at nine hertz has nothing
        // left for the fraction of a turn that is the whole answer.
        const auto turns = static_cast<float>(std::fmod(static_cast<double>(frequency) * simulationTime, 1.0));

        return std::sin(2.0f * Shaders::PI * (turns + phase + static_cast<float>(index) * sBandPhase));
    }

    /// The sum of four bands of the ladder, the highest of them at `top` hertz, in `-1 .. 1`.
    float flame(double simulationTime, float top, float phase)
    {
        float sum = 0.0f;
        float frequency = top;

        for (int i = 0; i < sFlameBands; ++i)
        {
            sum += band(simulationTime, frequency, phase, i);
            frequency /= sBandRatio;
        }

        // **Equal weights, which is what makes the spectrum pink.** The bands are a geometric
        // ladder, so one weight each is one share of the power per octave — the spectrum a flame
        // has, and the reason this reads as a flame rather than as a wobble at one rate. Divided by
        // their count so that the sum cannot leave `-1 .. 1`, which is what bounds the brightness.
        return sum / static_cast<float>(sFlameBands);
    }

    osg::Vec3f decodeColour(std::uint32_t packed)
    {
        const auto channel = [](std::uint32_t bits) { return toLinear(static_cast<float>(bits & 0xFFu) / 255.0f); };

        return osg::Vec3f(channel(packed), channel(packed >> 8), channel(packed >> 16));
    }

    std::optional<Light> makeLight(const osg::Vec3f& colour, float radius, const osg::Vec3f& position)
    {
        // The radius comes off a file something else wrote, or off a graph something else built, so
        // a nonsensical one is data rather than a broken contract: a light with no size lights
        // nothing and is dropped.
        if (!(radius > 0.0f))
            return std::nullopt;

        // **A light that subtracts is not one a ray can reach.** Negative illumination is a trick
        // for a renderer accumulating into a framebuffer, and it arrives here as a colour with a
        // negative channel — which is what `SceneUtil::createLightSource` builds out of a `Negative`
        // record, and what `makeLight(const SceneUtil::LightCommon&)` builds to match. Said here, where the two
        // paths meet, so neither can come to a different answer about the same lamp.
        if (colour.x() < 0.0f || colour.y() < 0.0f || colour.z() < 0.0f)
            return std::nullopt;

        return Light{
            .mPosition = position,
            .mIntensity = colour * (radius * radius * sIntensity),
            .mReach = radius * sReachScale + sReachBonus,

            // **A sixteenth is an estimate**, and the paragraph above argues it is a good one — a
            // lamp that casts no penumbra at all is the worse answer.
            .mSourceRadius = radius * sSourceFraction,
            .mClearance = radius * sFittingFraction,
        };
    }

    float lightBrightness(SceneUtil::LightController::LightType type, int id, double simulationTime)
    {
        const float phase = lightPhase(id);

        switch (type)
        {
            case SceneUtil::LightController::LT_Normal:
                return 1.0f;
            case SceneUtil::LightController::LT_Flicker:
                // The whole flame, puffing included. The content gives this one to open fires: a
                // tiki torch, a brazier, a spark shower and a failing Dwemer tube.
                return 1.0f + sFlameDepth * flame(simulationTime, sTopBand, phase);
            case SceneUtil::LightController::LT_FlickerSlow:
                // **The same flame with its puffing damped away**, which is what a flame behind
                // lantern glass, high on a wall or across a room actually shows: the drift is left,
                // and one slower band arrives under it. The window down the ladder is the whole
                // difference between the two, and it is what makes this one cross its own mean about
                // a third as often.
                return 1.0f + sFlameDepth * flame(simulationTime, sTopBand / sBandRatio, phase);
            case SceneUtil::LightController::LT_Pulse:
                return 1.0f + sPulseDepth * band(simulationTime, sPulseBand * sBandRatio, phase, 0);
            case SceneUtil::LightController::LT_PulseSlow:
                return 1.0f + sPulseDepth * band(simulationTime, sPulseBand, phase, 0);
        }

        return 1.0f;
    }

    osg::Vec3f lightColour(const SceneUtil::LightSource& source, double simulationTime)
    {
        const SceneUtil::LightController* animation = source.getController();

        // **The controller's colours where there is one, and the light's own where there is not.**
        // Every light the content places is built by `SceneUtil::createLightSource`, which hands the
        // record's colour to a controller and lets it write the frame's colour from there; a light
        // built by hand — the glow of a Light spell — has no controller and no animation, and what
        // it was given is what it radiates.
        const SceneUtil::Light& light = *source.getLight(0);
        const osg::Vec4f diffuse = animation != nullptr ? animation->getDiffuse() : light.getDiffuse();

        const float brightness
            = animation != nullptr ? lightBrightness(animation->getType(), source.getId(), simulationTime) : 1.0f;

        // **The fade reaches the ambient and the animation does not.** The animation is a flame's,
        // and the two places the game writes an ambient both mean a light with no flame in it: a
        // Light spell's glow puts its whole output there, and a lamp carried in a pack adds a white
        // one so that it lights its bearer. The glow is why the fade has to arrive — without it a
        // Light spell burns at full strength up to the frame the actor's node mask cuts, and then
        // goes out.
        const float fade = source.getActorFade();

        return decodeColour(diffuse) * (brightness * fade) + decodeColour(light.getAmbient()) * fade;
    }

    bool castsWherePlaced(const SceneUtil::LightCommon& record)
    {
        return !record.mOffDefault;
    }

    bool standLight(osg::Group& where, const SceneUtil::LightCommon& record, bool exterior)
    {
        if (!castsWherePlaced(record))
            return false;

        // **The mirror does not filter on the mask**, so it decides nothing here. It is what the
        // game marks a light node with, so the two graphs look the same to anything that ever does.
        SceneUtil::addLight(&where, record, SceneUtil::Mask_Lighting, exterior);

        return true;
    }

    std::optional<Light> makeLight(const SceneUtil::LightCommon& record, const osg::Vec3f& position)
    {
        if (!castsWherePlaced(record))
            return std::nullopt;

        // **Described the way the graph describes it, so the test above answers for both.**
        // `SceneUtil::createLightSource` turns a `Negative` record into a light by negating its
        // diffuse, and this negates the colour it decoded. The two do that on opposite sides of the
        // sRGB conversion, so what they agree on is the *sign* and not the magnitude — which is the
        // whole of what a refusal reads, and the magnitude of a light nobody places is nothing.
        //
        // The flag is read here as what it is, a property of the record, and not as a second rule
        // about what may be placed.
        const osg::Vec3f recorded = decodeColour(record.mColor);

        return makeLight(record.mNegative ? -recorded : recorded, record.mRadius, position);
    }

    Daylight makeRoomLight(const ESM::Cell::AMBIstruct& room, const osg::Vec3f& nightEye)
    {
        const osg::Vec3f haze = decodeColour(room.mFog);
        const osg::Vec3f fill = decodeColour(SceneUtil::colourFromRGB(room.mAmbient) + osg::Vec4f(nightEye, 0.0f));

        // The record's sunlight, kept whole and put where light with no direction belongs — the same
        // move `makeSkylight` makes on the night's sun, for the same reason and by the same factor.
        const osg::Vec3f spread = decodeColour(room.mSunlight) * (Shaders::DAYLIGHT * Shaders::INV_FOUR_PI);

        return Daylight{
            .mSkyHorizon = haze,
            .mSkyZenith = haze,
            .mAmbient = fill + spread,
            .mStarFade = 0.0f,
            .mExposureBias = 1.0f,
            .mFog = roomFog(haze, room.mFogDensity),
        };
    }
}
