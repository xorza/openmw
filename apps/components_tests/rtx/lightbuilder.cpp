#include <cmath>

#include <gtest/gtest.h>

#include <components/esm3/loadligh.hpp>
#include <components/esm3/loadregn.hpp>
#include <components/fallback/fallback.hpp>
#include <components/rtx/lightbuilder.hpp>
#include <components/rtx/shaders/visibility.h>
#include <components/sceneutil/util.hpp>
#include <components/weather/downpour.hpp>

namespace Rtx
{
    namespace
    {
        ESM::Light makeRecord(std::int32_t radius, std::uint32_t colour, std::int32_t flags)
        {
            ESM::Light record;
            record.mData.mRadius = radius;
            record.mData.mColor = colour;
            record.mData.mFlags = flags;
            return record;
        }

        /// The packing is `0xAABBGGRR`: red in the low byte.
        ///
        /// Reading it the other way round turns every candle in the game blue, which is the kind of
        /// wrong that looks deliberate.
        TEST(RtxLightBuilderTest, aColourIsRedFirstAndDecodedOutOfDisplaySpace)
        {
            EXPECT_EQ(decodeColour(0x00FFFFFF), osg::Vec3f(1.0f, 1.0f, 1.0f));
            EXPECT_EQ(decodeColour(0), osg::Vec3f(0.0f, 0.0f, 0.0f));

            const osg::Vec3f candle = decodeColour(0x000080FF);
            EXPECT_FLOAT_EQ(candle.x(), 1.0f) << "red is the low byte";
            EXPECT_EQ(candle.z(), 0.0f) << "and blue the third";

            // Mid grey is where the two spaces diverge most, so it is where skipping the decode is
            // most visible: 128 of 255 is 0.50196 encoded and
            // ((0.50196 + 0.055) / 1.055)^2.4 = 0.21586 linear.
            EXPECT_NEAR(candle.y(), 0.21586f, 1e-5f);
            EXPECT_NEAR(decodeColour(0x00808080).x(), 0.21586f, 1e-5f);
        }

        /// The colour the game hands over is the same colour, and takes the same decode.
        ///
        /// **OpenMW's own comment calls its pipeline linear and it is not the numbers it is talking
        /// about.** `SceneUtil::colourFromRGB` divides a record's bytes by 255 and stops, so what
        /// settles on a light, a fog or the sky is display-encoded exactly as the record was — which
        /// is why the game path decodes rather than passing it through, and why the two must land on
        /// the same value for the same record or a screenshot and the game are two different worlds.
        TEST(RtxLightBuilderTest, aColourTheGameHasAlreadyUnpackedDecodesToTheSameLight)
        {
            for (const std::uint32_t packed : { 0x00000000u, 0x00808080u, 0x000080FFu, 0x00FFFFFFu })
                EXPECT_EQ(decodeColour(packed), decodeColour(SceneUtil::colourFromRGB(packed))) << "packed " << packed;

            // The alpha is dropped rather than carried: nothing downstream of a light has a use for
            // one, and a fog colour arrives with its own.
            EXPECT_EQ(decodeColour(osg::Vec4f(1.0f, 1.0f, 1.0f, 0.25f)), osg::Vec3f(1.0f, 1.0f, 1.0f));

            // The same mid grey, reached the other way: 128 of 255 encoded is 0.21586 linear.
            EXPECT_NEAR(decodeColour(osg::Vec4f(128.0f / 255.0f, 0.0f, 0.0f, 1.0f)).x(), 0.21586f, 1e-5f);
        }

        /// Brightness, reach and the size of the flame all come off the one number the record
        /// carries, and part company.
        ///
        /// Intensity stays on the recorded radius, because that is what the lamp *is*. Only the
        /// falloff's run is stretched, because Morrowind's radii were tuned for a renderer where an
        /// ambient term lit the room and a lamp only had to light its own post. And the glowing part
        /// is a fraction of it, which is the same reading of the record as the intensity's: an
        /// emitter of fixed radiance is brighter by its area, so a lamp that is four times as bright
        /// is twice as wide and its shadows are twice as soft.
        TEST(RtxLightBuilderTest, intensityScalesWithTheRecordedRadiusAndReachIsStretchedPastIt)
        {
            const std::optional<Rtx::Light> light = makeLight(makeRecord(100, 0x00FFFFFF, 0), osg::Vec3f(1, 2, 3));

            ASSERT_TRUE(light.has_value());
            EXPECT_EQ(light->mPosition, osg::Vec3f(1, 2, 3));

            // 100 * 100 * 0.25 * pi = 7853.98, and white decodes to one.
            EXPECT_NEAR(light->mIntensity.x(), 7853.98f, 0.01f);
            EXPECT_NEAR(light->mIntensity.y(), 7853.98f, 0.01f);

            // 100 * 2 + 128.
            EXPECT_FLOAT_EQ(light->mReach, 328.0f);

            // A sixteenth of the record: 6.25 units, which is nine centimetres across at seventy
            // units to the metre — a flame, and not the metre and a half the reach describes.
            EXPECT_FLOAT_EQ(light->mRadius, 6.25f);

            // Doubling the radius quadruples the brightness, doubles the flame and rather less than
            // doubles the reach: 200 * 200 * 0.25 * pi = 31415.9, 200 / 16 = 12.5, and
            // 200 * 2 + 128 = 528.
            const std::optional<Rtx::Light> larger = makeLight(makeRecord(200, 0x00FFFFFF, 0), osg::Vec3f());
            ASSERT_TRUE(larger.has_value());
            EXPECT_NEAR(larger->mIntensity.x(), 31415.9f, 0.1f);
            EXPECT_FLOAT_EQ(larger->mReach, 528.0f);
            EXPECT_FLOAT_EQ(larger->mRadius, 12.5f);

            // The two readings of the record are one reading: an emitter of fixed radiance is
            // brighter by its area, so the brightness has to be the square of the size for a candle
            // and a brazier to be the same fire at two scales rather than two arbitrary lamps.
            EXPECT_NEAR(larger->mIntensity.x() / light->mIntensity.x(),
                (larger->mRadius / light->mRadius) * (larger->mRadius / light->mRadius), 1e-4f);
        }

        /// The sun's arc, which is the engine's own and not an approximation of it.
        ///
        /// `(-400 * orbit, 75, -100)` with `orbit` running from one at sunrise to minus one at
        /// nightfall — so the vector is where the light *goes*, west at dawn and east at dusk, and
        /// Every quarter hour of the day, asked for.
        ///
        /// **A fallback key the game does not define throws rather than reading zero**, so this is a
        /// test that `makeDaylight` asks only for settings that exist. It did not: the land fog
        /// depth is recorded for day and night alone, and every hour inside sunrise or sunset asked
        /// for a third that was never written, which took the whole tool down.
        ///
        /// The times are seeded here because the phase boundaries come out of the same map, and an
        /// unseeded one puts sunrise and sunset on top of each other at midnight. `Fallback::Map`
        /// keeps the first value it is given for a key and a test elsewhere in this binary opens the
        /// real installation, so which values these reads got depends on the order the suite ran in
        /// — which is why what is pinned below is what is true of either.
        TEST(RtxLightBuilderTest, everyHourAsksOnlyForSettingsTheGameDefines)
        {
            Fallback::Map::init({
                { "Weather_Sunrise_Time", "6" },
                { "Weather_Sunset_Time", "18" },
                { "Weather_Sunset_Duration", "2" },
                { "Weather_Clear_Land_Fog_Day_Depth", "0.4" },
                { "Weather_Clear_Land_Fog_Night_Depth", "0.8" },
                { "Weather_Clear_Wind_Speed", "0.3" },
                { "Weather_Ashstorm_Wind_Speed", "0.8" },
                { "Weather_Clear_Sun_Disc_Sunset_Color", "255,189,157" },
                { "Weather_Clear_Glare_View", "1" },

                // The sun's own ramp, seeded with the shipped numbers so that the two ways this
                // test can be run — against these or against a real installation — agree. The night
                // value being the blue one is what the disc is here to not be painted with.
                { "Weather_Clear_Sun_Sunrise_Color", "242,159,119" },
                { "Weather_Clear_Sun_Day_Color", "255,252,238" },
                { "Weather_Clear_Sun_Sunset_Color", "255,114,079" },
                { "Weather_Clear_Sun_Night_Color", "059,097,176" },
            });

            for (float hour = 0.0f; hour < 24.0f; hour += 0.25f)
                EXPECT_NO_THROW(makeDaylight("Clear", hour)) << "at hour " << hour;

            // A file records one depth for daylight and one for night, and the ramp hands the day
            // value to three of its four points. Deeper fog is thicker air, so the night value being
            // the larger of the two is what makes these comparisons say different things.
            const float day = makeDaylight("Clear", 12.0f).mFog.mExtinction;
            const float night = makeDaylight("Clear", 0.0f).mFog.mExtinction;
            EXPECT_GT(night, day);
            EXPECT_EQ(makeDaylight("Clear", 6.0f).mFog.mExtinction, day) << "sunrise reads the day depth";
            EXPECT_EQ(makeDaylight("Clear", 20.0f).mFog.mExtinction, night) << "and night begins at twenty";

            // **Dusk is between the two rather than one of them**, which is the whole of what the
            // engine's own ramp buys over reading whichever phase an hour falls in: the seeded
            // sunset runs from eighteen to twenty, so half past seven is halfway across it.
            const float dusk = makeDaylight("Clear", 19.5f).mFog.mExtinction;
            EXPECT_GT(dusk, day);
            EXPECT_LT(dusk, night);

            // **A night has no sun in it at all**, which is one fact rather than the engine's two.
            // Morrowind never switches its sunlight off — `WeatherManager` reads a colour off the
            // same ramp all night and turns off only the sprite — and a tracer that kept that light
            // cast hard shadows swinging back across the ground until dawn, from a disc nothing was
            // drawing. There is no second field left to say otherwise.
            EXPECT_NE(makeDaylight("Clear", 12.0f).mSun.mIrradiance, osg::Vec3f()) << "noon";
            EXPECT_EQ(makeDaylight("Clear", 0.0f).mSun.mIrradiance, osg::Vec3f()) << "midnight";
            EXPECT_EQ(makeDaylight("Clear", 22.0f).mSun.mIrradiance, osg::Vec3f()) << "night begins at twenty";
            EXPECT_NE(makeDaylight("Clear", 7.0f).mSun.mIrradiance, osg::Vec3f()) << "and it is back after six";

            // **And what the file left in that slot is not lost, only turned into what it is.** A
            // Morrowind night is still lit; it is lit by something with no direction, so nothing in
            // it casts a shadow at a sun that is not there.
            EXPECT_GT(makeDaylight("Clear", 0.0f).mAmbient.z(), makeDaylight("Clear", 12.0f).mAmbient.z())
                << "the night's blue sun went into the ambient, and it is bluer than the day's";

            // The disc is white for every hour the sun is up and only warms on the way down, which
            // is the one thing the light never does.
            for (const float hour : { 6.5f, 9.0f, 12.0f, 15.0f })
                EXPECT_EQ(makeDaylight("Clear", hour).mSun.mDiscColour, osg::Vec3f(1.0f, 1.0f, 1.0f))
                    << "at hour " << hour;

            const Daylight down = makeDaylight("Clear", 18.0f);
            EXPECT_FLOAT_EQ(down.mSun.mDiscColour.x(), 1.0f);
            EXPECT_LT(down.mSun.mDiscColour.z(), down.mSun.mDiscColour.x()) << "warm on the way down, never blue";

            // The wind comes off the same file and a key per weather, so a storm reading harder
            // than fair weather is what says the name reached the lookup rather than a constant
            // being handed back.
            //
            // **Compared rather than pinned.** The seeds above are 0.3 and 0.8, but a test elsewhere
            // in this binary opens the real installation and `Fallback::Map::init` keeps whichever
            // value landed first — so which pair this reads depends on the order the suite ran in,
            // and only the inequality is true of both.
            EXPECT_GT(Weather::windSpeed("Ashstorm"), Weather::windSpeed("Clear"));
            EXPECT_GT(Weather::windSpeed("Clear"), 0.0f);

            // A name that is none of the ten is not a key the map will even consider, which is why
            // `weatherIndex` is the thing to ask first.
            EXPECT_THROW(makeDaylight("Drizzle", 12.0f), std::logic_error);
        }

        /// A sun below the horizon is not a sun, and its light is not lost either.
        ///
        /// **This is the one rule, and it is here so that a renderer cannot be written without it.**
        /// Every sun bug this file has seen was the same shape — the engine keeps five independent
        /// dials for one sun and its rasterizer never had to make two of them agree, so a tracer
        /// that carried them across got a light coming from one place, a disc drawn in another, and
        /// a shadow cast at an hour when nothing was drawn at all. `makeSkylight` is the only way to
        /// build one, and there is nothing it can be handed that says the incoherent thing.
        TEST(RtxSkylightTest, aSunBelowTheHorizonLightsNothingAndItsLightBecomesTheAmbient)
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

            // **And the night is still lit.** What the file left in the sun's slot goes where light
            // with no direction belongs, so a night loses its shadows rather than its brightness:
            // a quarter of the irradiance over pi, which is a directional averaged over every
            // orientation a surface could take.
            const osg::Vec3f fill = blue * Rtx::Shaders::DAYLIGHT * (0.25f / Rtx::Shaders::PI);
            EXPECT_EQ(at(0.0f).mAmbient, room + fill);
            EXPECT_EQ(at(1.0f).mAmbient, room) << "and a day's ambient is untouched";

            // **The two halves are complements**, so the light in the scene does not step when the
            // sun goes out at the end of dusk: what leaves the direction arrives in the fill.
            for (const float share : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
            {
                const Skylight sky = at(share);
                const osg::Vec3f total = sky.mSun.mIrradiance * (0.25f / Rtx::Shaders::PI) + sky.mAmbient;
                for (int channel = 0; channel < 3; ++channel)
                    EXPECT_NEAR(total[channel], (room + fill)[channel], 1e-6f) << "at share " << share;
            }

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
        }

        /// The ten names, in the order a script id counts along.
        ///
        /// **This order is the engine's and not ours.** `MWWorld::WeatherManager::addWeather` is
        /// called ten times in `apps/openmw/mwworld/weather.cpp:672` and each call's position is the
        /// `mScriptId` the game later hands the renderer; the shader's `WEATHER_*` name the same
        /// positions. A table that drifted from either would put an ashstorm's sky over a rainstorm
        /// without anything failing to compile.
        TEST(RtxLightBuilderTest, aWeatherNameIndexesTheOrderTheEngineRegistersThemIn)
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
            // second here would hand `makeDaylight` a name that throws.
            EXPECT_FALSE(weatherIndex("clear").has_value());
            EXPECT_FALSE(weatherIndex("").has_value());
        }

        /// A region is offered only the weathers it ever gets.
        ///
        /// **A `REGN` record's ten chances add to a hundred and a zero means never**, in the order
        /// `WEATHER_*` names them. The Bitter Coast has no ashstorms and the Ashlands no snow, so a
        /// window that walked all ten would offer skies the game could not produce there.
        TEST(RtxLightBuilderTest, steppingTheWeatherSkipsTheOnesTheRegionNeverGets)
        {
            // Clear, Cloudy and Rain only — the shape of a coastal region, with everything from
            // Thunderstorm on left at nothing.
            ESM::Region coast;
            coast.mData.mProbabilities = { 50, 30, 0, 0, 20, 0, 0, 0, 0, 0 };

            EXPECT_EQ(nextRegionWeather(&coast, Rtx::Shaders::WEATHER_CLEAR, true), Rtx::Shaders::WEATHER_CLOUDY);
            EXPECT_EQ(nextRegionWeather(&coast, Rtx::Shaders::WEATHER_CLOUDY, true), Rtx::Shaders::WEATHER_RAIN)
                << "Foggy and Overcast are skipped";
            EXPECT_EQ(nextRegionWeather(&coast, Rtx::Shaders::WEATHER_RAIN, true), Rtx::Shaders::WEATHER_CLEAR)
                << "and it wraps past the six it never gets";

            // Backwards over the same three.
            EXPECT_EQ(nextRegionWeather(&coast, Rtx::Shaders::WEATHER_CLEAR, false), Rtx::Shaders::WEATHER_RAIN);
            EXPECT_EQ(nextRegionWeather(&coast, Rtx::Shaders::WEATHER_RAIN, false), Rtx::Shaders::WEATHER_CLOUDY);
            EXPECT_EQ(nextRegionWeather(&coast, Rtx::Shaders::WEATHER_CLOUDY, false), Rtx::Shaders::WEATHER_CLEAR);

            // **A step from a weather the region does not get still lands on one it does**, which is
            // what a camera crossing out of one region into another leaves behind.
            EXPECT_EQ(nextRegionWeather(&coast, Rtx::Shaders::WEATHER_BLIZZARD, true), Rtx::Shaders::WEATHER_CLEAR);

            // No region — an interior, or a cell naming one nothing defines — offers all ten.
            EXPECT_EQ(nextRegionWeather(nullptr, Rtx::Shaders::WEATHER_CLEAR, true), Rtx::Shaders::WEATHER_CLOUDY);
            EXPECT_EQ(nextRegionWeather(nullptr, Rtx::Shaders::WEATHER_CLEAR, false), Rtx::Shaders::WEATHER_BLIZZARD);

            // And a record that allows nothing at all steps once rather than spinning for ever.
            ESM::Region nowhere;
            nowhere.mData.mProbabilities = {};
            EXPECT_EQ(nextRegionWeather(&nowhere, Rtx::Shaders::WEATHER_CLEAR, true), Rtx::Shaders::WEATHER_CLOUDY);
        }

        /// Ash and blight blow off Red Mountain at whoever is standing in them.
        ///
        /// `apps/openmw/mwworld/weather.cpp:47` takes the direction from the volcano at (25000,
        /// 70000) to the player, flattened to the ground. Every other weather leaves it due north,
        /// which is `Weather::defaultStormDirection`.
        TEST(RtxLightBuilderTest, anAshStormBlowsAwayFromRedMountainAndNothingElseTurnsAtAll)
        {
            const osg::Vec3f north(0.0f, 1.0f, 0.0f);

            // A three-four-five triangle off the summit, so the unit vector is exact: (3, 4) over a
            // length of 5 is (0.6, 0.8). The height is thrown away rather than normalised with the
            // rest, which is what keeps the wind on the ground.
            const osg::Vec3f standing(25003.0f, 70004.0f, 999.0f);
            for (const std::uint32_t weather : { Rtx::Shaders::WEATHER_ASHSTORM, Rtx::Shaders::WEATHER_BLIGHT })
            {
                const osg::Vec3f blowing = stormDirection(weather, standing);
                EXPECT_FLOAT_EQ(blowing.x(), 0.6f) << "weather " << weather;
                EXPECT_FLOAT_EQ(blowing.y(), 0.8f) << "weather " << weather;
                EXPECT_FLOAT_EQ(blowing.z(), 0.0f) << "weather " << weather;
            }

            // Due south of the mountain it points south, which is the half of "away from" that a
            // fixed bearing would get wrong.
            EXPECT_EQ(stormDirection(Rtx::Shaders::WEATHER_ASHSTORM, osg::Vec3f(25000.0f, 60000.0f, 0.0f)),
                osg::Vec3f(0.0f, -1.0f, 0.0f));

            // Standing on the summit there is no away, and a normalised zero is a frame of NaN.
            EXPECT_EQ(stormDirection(Rtx::Shaders::WEATHER_ASHSTORM, osg::Vec3f(25000.0f, 70000.0f, 4000.0f)), north);

            // Everything the mountain does not send reads the wind's own bearing wherever it stands.
            for (const std::uint32_t weather : { Rtx::Shaders::WEATHER_CLEAR, Rtx::Shaders::WEATHER_RAIN,
                     Rtx::Shaders::WEATHER_BLIZZARD, Rtx::Shaders::WEATHER_SNOW })
                EXPECT_EQ(stormDirection(weather, standing), north) << "weather " << weather;
        }

        /// Three kinds of record place a mesh and no light, and one kind is nonsense.
        TEST(RtxLightBuilderTest, carriedNegativeAndUnlitRecordsCastNothing)
        {
            for (const std::int32_t flag : { ESM::Light::Carry, ESM::Light::Negative, ESM::Light::OffDefault })
                EXPECT_FALSE(makeLight(makeRecord(100, 0x00FFFFFF, flag), osg::Vec3f()).has_value()) << "flag " << flag;

            // The flags that only say how a light animates leave it burning.
            for (const std::int32_t flag : { ESM::Light::Flicker, ESM::Light::Fire, ESM::Light::Pulse })
                EXPECT_TRUE(makeLight(makeRecord(100, 0x00FFFFFF, flag), osg::Vec3f()).has_value()) << "flag " << flag;

            // A file on disk that something else wrote, so a radius of nothing is data and not a
            // broken contract.
            EXPECT_FALSE(makeLight(makeRecord(0, 0x00FFFFFF, 0), osg::Vec3f()).has_value());
            EXPECT_FALSE(makeLight(makeRecord(-50, 0x00FFFFFF, 0), osg::Vec3f()).has_value());
        }
    }
}
