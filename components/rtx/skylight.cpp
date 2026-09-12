#include "skylight.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include <components/sceneutil/util.hpp>
#include <components/sky/sun.hpp>

#include "decodecolour.hpp"
#include "shaders/colour.h"
#include "shaders/look.h"
#include "shaders/scene.h"
#include "shaders/visibility.h"

namespace Rtx
{
    namespace
    {
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

        /// A sun out of a weather's reading and however much of the disc the asker can see.
        ///
        /// **The ground's share and a layer's are the same sun**, which is what makes this one
        /// function: a cloud deck stands above the ground's horizon and keeps the sun after it has
        /// set down here, and everything else about it is the same — the same place, and the
        /// content's own sunset colour, which is keyed on the hour rather than on how much air the
        /// beam crossed.
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

        // **Against a full sun rather than against a noon worked out here.** A clear noon
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
        // the frame. `2 * s * (1 - s)` is that, and the two puts a dusk at a half at the half-set
        // point.
        //
        // **The shape that suggests itself is `1 - share`, and it is largest where there is no sun.**
        // Morrowind leaves a blue in the sun's slot all night — `Sun_Night_Color`, which is the
        // original engine's stand-in for moonlight — and spreading that as an ambient came to six
        // times the night ambient the weather itself records, flat, with no direction and no shadow
        // in it. This renderer traces the moons, so keeping it is the moon counted twice and a night
        // that does not read as one.
        const float dusk = 2.0f * share * (1.0f - share);

        Skylight light{
            .mSun = sunAbove(sky, share),
            .mSunAloft = sunAbove(sky, sky.mSunShareAloft),
            .mAmbient = sky.mAmbient + irradiance * (dusk * Shaders::INV_FOUR_PI),
        };

        // After both terms, because it measures what they come to between them.
        light.mExposureBias = exposureBias(light.mSun.mIrradiance, light.mAmbient);

        return light;
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

    std::optional<std::uint32_t> weatherIndex(std::string_view weather)
    {
        const auto found = std::find(sWeathers.begin(), sWeathers.end(), weather);
        if (found == sWeathers.end())
            return std::nullopt;

        return static_cast<std::uint32_t>(found - sWeathers.begin());
    }

    std::string_view weatherName(std::uint32_t weather)
    {
        return weather < sWeathers.size() ? sWeathers[weather] : std::string_view();
    }

    Daylight makeRoomLight(const ESM::Cell::AMBIstruct& room, const osg::Vec3f& nightEye)
    {
        const osg::Vec3f haze = decodeColour(room.mFog);
        const osg::Vec3f fill = decodeColour(SceneUtil::colourFromRGB(room.mAmbient) + osg::Vec4f(nightEye, 0.0f));

        // The record's sunlight, kept whole and put where light with no direction belongs — the same
        // move `makeSkylight` makes on the night's sun, for the same reason and by the same factor.
        const osg::Vec3f spread = decodeColour(room.mSunlight) * (Shaders::DAYLIGHT * Shaders::INV_FOUR_PI);

        return Daylight{
            .mLight = Skylight{ .mAmbient = fill + spread, .mExposureBias = 1.0f },
            .mSkyHorizon = haze,
            .mSkyZenith = haze,
            .mStarFade = 0.0f,
            .mFog = roomFog(haze, room.mFogDensity),
        };
    }
}
