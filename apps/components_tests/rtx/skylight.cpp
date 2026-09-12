#include <cstdint>

#include <gtest/gtest.h>

#include <osg/Vec3f>

#include <components/esm3/loadcell.hpp>
#include <components/rtx/colour.hpp>
#include <components/rtx/fogbuilder.hpp>
#include <components/rtx/shaders/look.h>
#include <components/rtx/shaders/scene.h>
#include <components/rtx/shaders/visibility.h>
#include <components/rtx/skylight.hpp>
#include <components/sky/sun.hpp>
#include <components/sky/timeofday.hpp>

namespace Rtx
{
    namespace
    {
        /// A sun below the horizon is not a sun, in either term.
        ///
        /// **This is the one rule, and it is here so that a renderer cannot be written without it.**
        /// Every sun bug this file has seen was the same shape — the engine keeps five independent
        /// dials for one sun and its rasterizer never had to make two of them agree, so a tracer
        /// that carried them across got a light coming from one place, a disc drawn in another, and
        /// a shadow cast at an hour when nothing was drawn at all. `makeSkylight` is the only way to
        /// build one, and there is nothing it can be handed that says the incoherent thing.
        TEST(RtxSkylightTest, aSunBelowTheHorizonLightsNothingInEitherTerm)
        {
            const osg::Vec3f up(0.0f, 0.0f, 1.0f);
            const osg::Vec3f blue(0.05f, 0.12f, 0.44f); ///< what `Sun_Night_Color` decodes to
            const osg::Vec3f room(0.01f, 0.011f, 0.013f);

            const auto at = [&](float share) {
                return makeSkylight(
                    SkyReading{ .mSunPosition = up, .mSunShare = share, .mSunColour = blue, .mAmbient = room });
            };

            // **Nothing at all when there is no sun**, and it is the irradiance that says so, since
            // that is the one thing every use of the sun downstream is gated on.
            EXPECT_EQ(at(0.0f).mSun.mIrradiance, osg::Vec3f());

            // The whole of it when there is, on the shared sun-to-sky scale.
            EXPECT_EQ(at(1.0f).mSun.mIrradiance, blue * Rtx::Shaders::DAYLIGHT);

            // **And the fill is a dusk's, not a night's.** The sun's light with its direction taken
            // away is a thing only an hour with a sun in it has — so this is nothing at either end
            // and largest where the disc straddles the horizon.
            //
            // **The shape that suggests itself is `1 - share`, and it is largest where there is no sun.**
            // Morrowind leaves a blue in the sun's slot all night, and it is the original engine's
            // stand-in for moonlight; spread as an ambient it came to six times the night ambient the
            // weather itself records, flat and shadowless, on every surface. This renderer traces the
            // moons, so keeping it was the moon counted twice and a night that did not read as one.
            EXPECT_EQ(at(0.0f).mAmbient, room) << "a night's ambient is the weather's own";
            EXPECT_EQ(at(1.0f).mAmbient, room) << "and a day's is untouched";

            // Half set: `2 * 0.5 * 0.5` is a half of a quarter of the irradiance over pi.
            const osg::Vec3f half = blue * Rtx::Shaders::DAYLIGHT * (0.5f * 0.25f / Rtx::Shaders::PI);
            for (int channel = 0; channel < 3; ++channel)
                EXPECT_NEAR(at(0.5f).mAmbient[channel], (room + half)[channel], 1e-6f);

            // And it only ever adds, at every share between the two ends.
            for (const float share : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
                for (int channel = 0; channel < 3; ++channel)
                    EXPECT_GE(at(share).mAmbient[channel], room[channel]) << "at share " << share;

            // The position stays whatever it was, because a moon's crescent points at where the sun
            // would be and that is a different question from whether it is there.
            EXPECT_EQ(at(0.0f).mSun.mPosition, up);

            // A weather that hides the sun paints a paler disc, and says nothing about whether there
            // is one: the glare reaches only the colour.
            const Skylight overcast = makeSkylight(SkyReading{ .mSunPosition = up,
                .mSunShare = 1.0f,
                .mSunColour = blue,
                .mAmbient = room,
                .mDiscColour = osg::Vec3f(1.0f, 1.0f, 1.0f),
                .mGlare = 0.25f });
            EXPECT_EQ(overcast.mSun.mDiscColour, osg::Vec3f(0.25f, 0.25f, 0.25f));
            EXPECT_EQ(overcast.mSun.mIrradiance, blue * Rtx::Shaders::DAYLIGHT) << "and lights the same";

            // **The same weather read twice, at two shares.** A cloud deck stands above the ground's
            // horizon and keeps the sun after it has set down here, so it takes the same sun at its
            // own share — the same place, the same colour, and only the quantity apart.
            const Skylight dusk = makeSkylight(SkyReading{
                .mSunPosition = up, .mSunShare = 0.0f, .mSunShareAloft = 0.25f, .mSunColour = blue, .mAmbient = room });

            EXPECT_EQ(dusk.mSun.mIrradiance, osg::Vec3f()) << "the ground has lost it";
            EXPECT_EQ(dusk.mSunAloft.mIrradiance, blue * (0.25f * Rtx::Shaders::DAYLIGHT));
            EXPECT_EQ(dusk.mSunAloft.mPosition, dusk.mSun.mPosition);
            EXPECT_EQ(dusk.mSunAloft.mDiscColour, dusk.mSun.mDiscColour);

            // **And the ambient is the ground's alone.** What a layer above it keeps is that layer's
            // to spend; spreading it over the world as well would be the same light twice.
            EXPECT_EQ(dusk.mAmbient, room);
        }

        /// A layer over the ground keeps the sun, and what it keeps is an hour and not an angle.
        ///
        /// **Morrowind's sunset is a clock.** `Sky::sunShareAt` ramps between `mDayEnd` and
        /// `mNightStart` and nothing anywhere takes an elevation, so a layer that sees the sun 0.718
        /// degrees longer is handed the ramp read 5.35 minutes earlier — the time the disc takes to
        /// fall that far at 8.04 degrees an hour.
        ///
        /// **And the day is widened at both ends rather than moved**: a layer that sees the sun lower
        /// catches the sunrise early too, so the same offset runs the other way before noon.
        ///
        /// With Morrowind's own hours: at 19:00 the ground has 0.750 of the sun and the layer 0.793;
        /// at 20:00 the ground has none and the layer still holds 0.0872; and five minutes later the
        /// layer has none either.
        TEST(RtxSunAloftTest, aLayerOverTheGroundKeepsTheSunAfterItHasLostIt)
        {
            Sky::TimeOfDaySettings times{};
            times.mNightEnd = 6.0f;
            times.mDayStart = 8.0f;
            times.mDayEnd = 18.0f;
            times.mNightStart = 20.0f;

            EXPECT_NEAR(sunShareAloft(19.0f, times), 0.792623f, 1.0e-5f);
            EXPECT_NEAR(sunShareAloft(20.0f, times), 0.087237f, 1.0e-5f);
            EXPECT_EQ(sunShareAloft(20.1f, times), 0.0f) << "and it goes out too, five minutes later";

            // And the same offset the other way at dawn: at 06:05 the ground has a twentieth of the
            // sun and the layer has 0.139, because it caught the sunrise five minutes ago.
            EXPECT_NEAR(sunShareAloft(6.05f, times), 0.139227f, 1.0e-5f);

            // Nothing at all is different while the whole disc is up, which is every hour of the day
            // between the two ramps.
            for (const float hour : { 9.0f, 12.0f, 17.9f })
                EXPECT_EQ(sunShareAloft(hour, times), Sky::sunShareAt(hour, times)) << "at hour " << hour;

            // **And never less than the ground's**, at any hour of the clock — which is the whole
            // claim, and the one a sign error in the offset would break.
            for (float hour = 0.0f; hour < 24.0f; hour += 0.05f)
                EXPECT_GE(sunShareAloft(hour, times), Sky::sunShareAt(hour, times)) << "at hour " << hour;
        }

        /// The ten names, in the order a script id counts along.
        ///
        /// **This order is the engine's and not ours.** `MWWorld::WeatherManager::addWeather` is
        /// called ten times in `apps/openmw/mwworld/weather.cpp:672` and each call's position is the
        /// `mScriptId` the game later hands the renderer; the shader's `WEATHER_*` name the same
        /// positions. A table that drifted from either would put an ashstorm's sky over a rainstorm
        /// without anything failing to compile.
        TEST(RtxSkylightTest, aWeatherNameIndexesTheOrderTheEngineRegistersThemIn)
        {
            EXPECT_EQ(weatherIndex("Clear"), Rtx::Shaders::WEATHER_CLEAR);
            EXPECT_EQ(weatherIndex("Cloudy"), Rtx::Shaders::WEATHER_CLOUDY);
            EXPECT_EQ(weatherIndex("Foggy"), Rtx::Shaders::WEATHER_FOGGY);
            EXPECT_EQ(weatherIndex("Overcast"), Rtx::Shaders::WEATHER_OVERCAST);
            EXPECT_EQ(weatherIndex("Rain"), Rtx::Shaders::WEATHER_RAIN);
            EXPECT_EQ(weatherIndex("Thunderstorm"), Rtx::Shaders::WEATHER_THUNDERSTORM);
            EXPECT_EQ(weatherIndex("Ashstorm"), Rtx::Shaders::WEATHER_ASHSTORM);
            EXPECT_EQ(weatherIndex("Blight"), Rtx::Shaders::WEATHER_BLIGHT);
            EXPECT_EQ(weatherIndex("Snow"), Rtx::Shaders::WEATHER_SNOW);
            EXPECT_EQ(weatherIndex("Blizzard"), Rtx::Shaders::WEATHER_BLIZZARD);

            EXPECT_FALSE(weatherIndex("Drizzle").has_value());

            // **Case is not folded**, because the name goes on to spell a `Weather_<name>_*` key
            // and the fallback map's whitelist holds exactly one spelling of each. Accepting a
            // second here would turn a stop's sky to a weather the settings never wrote.
            EXPECT_FALSE(weatherIndex("clear").has_value());
            EXPECT_FALSE(weatherIndex("").has_value());
        }

        /// The hour holds an exposure back, and a noon does not.
        ///
        /// **The histogram cannot tell a midnight from a noon**, because it normalises whatever it
        /// is shown toward the key — so a renderer left to measure its own exposure has no night in
        /// it at any hour. The weather knows the hour absolutely, and this is what it says about it.
        TEST(RtxSkylightTest, theHourHoldsAnExposureBackAndANoonDoesNot)
        {
            // A full sun's worth of light is the hour the bias leaves alone, and anything past it is
            // held at one: an hour is allowed to darken a frame and never to open it.
            EXPECT_FLOAT_EQ(exposureBias(osg::Vec3f(8.0f, 8.0f, 8.0f), osg::Vec3f()), 1.0f);
            EXPECT_FLOAT_EQ(exposureBias(osg::Vec3f(80.0f, 80.0f, 80.0f), osg::Vec3f()), 1.0f);

            // A clear midnight: no sun at all, and the weather's own night ambient, which comes to
            // 0.0168 by luminance. `(0.0168 / 8)^0.314 = 0.14428`, which is the two stops and four
            // fifths the exponent comes to.
            EXPECT_NEAR(exposureBias(osg::Vec3f(), osg::Vec3f(0.0168f, 0.0168f, 0.0168f)), 0.14428f, 1e-4f);

            // And it only ever moves one way, so no hour is darker in the picture than a darker one.
            float darker = 0.0f;
            for (const float level : { 0.01f, 0.1f, 1.0f, 4.0f, 8.0f })
            {
                const float bias = exposureBias(osg::Vec3f(level, level, level), osg::Vec3f());
                EXPECT_GT(bias, darker) << "at a level of " << level;
                darker = bias;
            }
        }

        /// The air takes a body in the sky out as it goes down, and takes the blue out first.
        ///
        /// **Two published figures and nothing else.** Rayleigh optical depth at the three sRGB
        /// primaries is 0.0683, 0.0973 and 0.2213 — `0.008569 λ^-4` with its usual correction, at
        /// 600, 550 and 450 nanometres — and Kasten and Young's air mass runs from 0.9997 overhead
        /// to 37.92 at the horizon. The transmittance is `exp(-depth * mass)`, worked out below by
        /// hand.
        TEST(RtxSkylightTest, theAirTakesABodyOutAsItGoesDown)
        {
            // Overhead: one air mass, so the depths come through as they are. A ninth of the green
            // goes even there, which is the price a full moon at the zenith pays.
            const osg::Vec3f zenith = airTransmittance(1.0f);
            EXPECT_NEAR(zenith.x(), 0.9340f, 1e-4f);
            EXPECT_NEAR(zenith.y(), 0.9073f, 1e-4f);
            EXPECT_NEAR(zenith.z(), 0.8015f, 1e-4f);

            // Thirty degrees is 1.9943 air masses, which is where a moon is most of the way to
            // itself.
            const osg::Vec3f third = airTransmittance(0.5f);
            EXPECT_NEAR(third.x(), 0.8727f, 1e-4f);
            EXPECT_NEAR(third.y(), 0.8236f, 1e-4f);
            EXPECT_NEAR(third.z(), 0.6432f, 1e-4f);

            // The horizon is 37.92 of them, and blue does not survive it: three parts in ten
            // thousand against a thirteenth of the red.
            const osg::Vec3f edge = airTransmittance(0.0f);
            EXPECT_NEAR(edge.x(), 0.0750f, 1e-4f);
            EXPECT_NEAR(edge.y(), 0.0250f, 1e-4f);
            EXPECT_NEAR(edge.z(), 0.000226f, 1e-6f);

            // Below the horizon there is no slant path to measure, so it holds at the horizon's own
            // rather than taking a root of a negative angle.
            EXPECT_EQ(airTransmittance(-0.5f), edge);

            // And it only ever rises, which is what keeps a body from brightening as it sets.
            float below = 0.0f;
            for (int step = 0; step <= 64; ++step)
            {
                const float carried = airTransmittance(float(step) / 64.0f).y();
                EXPECT_GT(carried, below) << "at a height of " << float(step) / 64.0f;
                below = carried;
            }
        }

        /// A night's sky lights with more than it is drawn with, and a day's does not.
        ///
        /// **Because Morrowind states the two in different places.** It lights a night by putting
        /// `Ambient_<weather>_Night_Color` on every surface and it draws that night by
        /// `Sky_<weather>_Night_Color`, and for a clear night the first is 0.0168 by luminance
        /// against the second's 0.0030. A renderer that lights the ground by tracing the sky is short
        /// by the difference, which is the whole of what this makes up.
        TEST(RtxSkylightTest, aNightsSkyLightsWithMoreThanItIsDrawnWith)
        {
            const auto grey = [](float value) { return osg::Vec3f(value, value, value); };
            const auto filled = [&](float horizon, float zenith, float sheets, float ambient) {
                return skyBudget(grey(horizon), grey(zenith), grey(sheets), grey(ambient)).mFill.x();
            };

            // A gradient linear in `sin(elevation)` delivers what a uniform sky of `h / 3 + 2z / 3`
            // would, so a horizon of 0.3 under a zenith of 0.6 is worth 0.5 — and an ambient of 0.8
            // asks for the 0.3 that is left.
            EXPECT_NEAR(filled(0.3f, 0.6f, 0.0f, 0.8f), 0.3f, 1e-6f);

            // The zenith is worth twice the horizon, which is what makes those two different skies:
            // the same pair the other way up is worth 0.4 and leaves 0.4 to ask for.
            EXPECT_NEAR(filled(0.6f, 0.3f, 0.0f, 0.8f), 0.4f, 1e-6f);

            // A sky that already carries the ambient asks for nothing, and one that outruns it does
            // not ask for less than nothing.
            EXPECT_EQ(filled(0.5f, 0.5f, 0.0f, 0.5f), 0.0f);
            EXPECT_EQ(filled(0.9f, 0.9f, 0.0f, 0.2f), 0.0f);

            // **The night's own sheets come out of the same figure**, which is what keeps a night
            // where it was as one more layer starts lighting: the stars used to light nothing, and a
            // tenth of the 0.3 above now comes from them instead of from the fill.
            EXPECT_NEAR(filled(0.3f, 0.6f, 0.03f, 0.8f), 0.27f, 1e-6f);
            EXPECT_EQ(filled(0.3f, 0.6f, 0.5f, 0.8f), 0.0f) << "and sheets that outrun it ask for nothing";

            // And it asks per channel: a red ambient over a grey sky fills the red alone rather than
            // lifting the whole of it.
            const SkyBudget tinted = skyBudget(grey(0.5f), grey(0.5f), osg::Vec3f(), osg::Vec3f(0.9f, 0.5f, 0.1f));
            EXPECT_NEAR(tinted.mFill.x(), 0.4f, 1e-6f);
            EXPECT_EQ(tinted.mFill.y(), 0.0f);
            EXPECT_EQ(tinted.mFill.z(), 0.0f);

            // **And the mean beside it is the whole of what the sky delivers**, fill and all — which
            // is what a cloud deck hangs under. The sky carries 0.5 by itself, so the red channel
            // reaches the 0.9 its ambient asks for and the other two stay at what the sky is.
            EXPECT_NEAR(tinted.mMean.x(), 0.9f, 1e-6f);
            EXPECT_NEAR(tinted.mMean.y(), 0.5f, 1e-6f);
            EXPECT_NEAR(tinted.mMean.z(), 0.5f, 1e-6f);
        }

        /// The sky settles its bias out of the terms it built, not the ones it was handed.
        ///
        /// **Which is the whole of what the statement order in `makeSkylight` carries.** A dusk has
        /// most of its light in the ambient rather than in the disc, and that share is put there by
        /// the spread — so a bias taken before it would hold a sunset back as though it were a
        /// night. `theHourHoldsAnExposureBackAndANoonDoesNot` is what the curve itself is pinned by.
        TEST(RtxSkylightTest, theSkySettlesItsBiasAfterTheDuskSpreadRatherThanBefore)
        {
            const osg::Vec3f recorded(0.05f, 0.05f, 0.05f);

            const Skylight dusk = makeSkylight(SkyReading{ .mSunPosition = osg::Vec3f(0.0f, 0.0f, 1.0f),
                .mSunShare = 0.5f,
                .mSunColour = osg::Vec3f(1.0f, 1.0f, 1.0f),
                .mAmbient = recorded });

            ASSERT_GT(dusk.mAmbient.x(), recorded.x()) << "the spread moved nothing, so the order decides nothing";

            EXPECT_FLOAT_EQ(dusk.mExposureBias, exposureBias(dusk.mSun.mIrradiance, dusk.mAmbient));
            EXPECT_NE(dusk.mExposureBias, exposureBias(dusk.mSun.mIrradiance, recorded));
        }

        /// A room's `AMBI` record, as Berandas, Propylon Chamber writes it: ambient `15, 15, 15`,
        /// sunlight `10, 16, 16`, fog `15, 21, 21` at a depth of one. Red is the low byte.
        ESM::Cell::AMBIstruct makeRoom(std::uint32_t ambient, std::uint32_t sunlight, std::uint32_t fog)
        {
            return ESM::Cell::AMBIstruct{
                .mAmbient = ambient, .mSunlight = sunlight, .mFog = fog, .mFogDensity = 1.0f
            };
        }

        /// A room is lit by its own record and by nothing else, and it has no sun in it.
        ///
        /// **The sunlight is light and not a sun.** `configureAmbient` aims a directional light down
        /// an arbitrary vector, which traced is a hard shadow off a direction the content never
        /// chose and a bright seam through every join in the shell — so the record's sunlight keeps
        /// its energy and loses its direction, over `INV_FOUR_PI`.
        ///
        /// A directional source of irradiance `E` delivers `E / 4` averaged over every orientation a
        /// surface can take, and a uniform hemisphere of radiance `L` delivers `pi L` to all of
        /// them, so `L = E / 4pi` is the same light with the direction taken out. `DAYLIGHT / 4pi`
        /// is `8 * 0.0795775 = 0.636620` of the decoded colour.
        ///
        /// A record's colour is `0x00BBGGRR`, so `0x0010100A` is a red of ten and a green and blue of
        /// sixteen. The ambient decodes `15 / 255 = 0.058824` to `((0.058824 + 0.055) / 1.055)^2.4 =
        /// 0.0047770` in every channel. Red: `10 / 255 = 0.039216` is under the curve's knee, so it
        /// decodes `0.039216 / 12.92 = 0.0030353` and the sum is
        /// `0.0047770 + 0.0030353 * 0.636620 = 0.0067093`. Green and blue: `16 / 255` decodes
        /// `0.0051815` and the sum is `0.0080756`.
        TEST(RtxRoomLightTest, aRoomIsLitByItsRecordAndHasNoSunInIt)
        {
            const ESM::Cell::AMBIstruct chamber = makeRoom(0x000F0F0F, 0x0010100A, 0x0015150F);
            const Daylight room = makeRoomLight(chamber);

            // **Nothing anywhere gates a room's sun on a second field**, so a zero irradiance is the
            // whole of it: no direct term, no shadow ray, no disc drawn, and the kernel's `HAS_SUN`
            // folds away. `Sun::mIrradiance` carries that invariant.
            EXPECT_EQ(room.mLight.mSun.mIrradiance, osg::Vec3f(0.0f, 0.0f, 0.0f));
            EXPECT_EQ(room.mLight.mSunAloft.mIrradiance, osg::Vec3f(0.0f, 0.0f, 0.0f)) << "and no deck over it";

            EXPECT_NEAR(room.mLight.mAmbient.x(), 0.0067093f, 1e-6f);
            EXPECT_NEAR(room.mLight.mAmbient.y(), 0.0080756f, 1e-6f);
            EXPECT_NEAR(room.mLight.mAmbient.z(), 0.0080756f, 1e-6f) << "blue shares the green byte";

            EXPECT_EQ(room.mSkyHorizon, decodeColour(0x0015150Fu));
            EXPECT_EQ(room.mSkyZenith, room.mSkyHorizon);

            const Fog air = roomFog(decodeColour(0x0015150Fu), 1.0f);
            EXPECT_EQ(room.mFog.mColour, air.mColour);
            EXPECT_FLOAT_EQ(room.mFog.mExtinction, air.mExtinction);

            EXPECT_FLOAT_EQ(room.mStarFade, 0.0f);
            EXPECT_FLOAT_EQ(room.mLight.mExposureBias, 1.0f) << "the game holds a room at one";

            // **The spread is the sunlight's doing and nothing else's**: the same room with its
            // sunlight written black keeps the record's ambient exactly, so what the row above adds
            // came from the `AMBI` and not from a floor put under every interior.
            const Daylight unlit = makeRoomLight(makeRoom(0x000F0F0F, 0x00000000, 0x0015150F));
            EXPECT_EQ(unlit.mLight.mAmbient, decodeColour(0x000F0F0Fu));
            EXPECT_LT(unlit.mLight.mAmbient.x(), room.mLight.mAmbient.x()) << "and the sunlight is worth something";

            // **Night-Eye is added where the game adds it**: to the file's own numbers, before the
            // decode, and to the ambient alone. `15 / 255 + 0.35 = 0.40882`, and
            // `((0.40882 + 0.055) / 1.055)^2.4 = 0.13914`, with the red channel's own share of the
            // sunlight on top.
            const Daylight seen = makeRoomLight(chamber, osg::Vec3f(0.35f, 0.35f, 0.35f));
            EXPECT_NEAR(seen.mLight.mAmbient.x(), 0.13914f + 0.0019323f, 2e-4f);

            // **A cell that wrote no record is a black room**, in the game and here: its `mAmbi` is
            // the zeros the loader left, and both hosts hand those over rather than checking
            // `mHasAmbi` first.
            ESM::Cell unwritten;
            unwritten.mHasAmbi = false;
            const Daylight bare = makeRoomLight(unwritten.mAmbi);
            EXPECT_EQ(bare.mLight.mSun.mIrradiance, osg::Vec3f(0.0f, 0.0f, 0.0f));
            EXPECT_EQ(bare.mLight.mAmbient, osg::Vec3f(0.0f, 0.0f, 0.0f));
            EXPECT_FLOAT_EQ(bare.mFog.mExtinction, 0.0f);
        }
    }
}
