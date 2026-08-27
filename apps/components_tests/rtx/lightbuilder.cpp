#include <cmath>

#include <gtest/gtest.h>

#include <components/esm3/loadligh.hpp>
#include <components/esm3/loadregn.hpp>
#include <components/fallback/fallback.hpp>
#include <components/rtx/lightbuilder.hpp>
#include <components/rtx/shaders/visibility.h>
#include <components/sceneutil/lightcommon.hpp>
#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/lightutil.hpp>
#include <components/sceneutil/util.hpp>
#include <components/sceneutil/vismask.hpp>
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

        osg::ref_ptr<SceneUtil::LightSource> makeGraphLight(const osg::Vec4f& diffuse, const osg::Vec4f& ambient)
        {
            osg::ref_ptr<SceneUtil::Light> light = new SceneUtil::Light;
            light->setDiffuse(diffuse);
            light->setAmbient(ambient);

            osg::ref_ptr<SceneUtil::LightSource> source = new SceneUtil::LightSource;
            source->setLight(light);
            return source;
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

        /// What a light in the graph radiates is both of its terms, decoded — and it has to be the
        /// same answer the record gives, or a played frame and a screenshot are lit differently.
        ///
        /// **The ambient is not a second kind of light here.** A fixed-function pipeline had two
        /// terms because it had two things to do with them; a tracer has one. `setLightEffect` puts
        /// a glow light's whole colour in the ambient and leaves the diffuse at zero, so reading the
        /// diffuse alone is reading every Light spell in the game as unlit.
        TEST(RtxLightBuilderTest, aGraphLightRadiatesBothItsTermsAndTakesTheRecordsDecode)
        {
            // 128 of 255 is 0.50196 encoded, and ((0.50196 + 0.055) / 1.055)^2.4 = 0.21586 linear.
            const osg::Vec4f grey(128.0f / 255.0f, 0.0f, 0.0f, 1.0f);
            EXPECT_NEAR(lightColour(*makeGraphLight(grey, osg::Vec4f())).x(), 0.21586f, 1e-5f);

            // What `Animation::setLightEffect` builds: nothing in the diffuse, 1.5 in the ambient.
            // ((1.5 + 0.055) / 1.055)^2.4 = 2.53716, and a walk reading the diffuse alone gets zero.
            const osg::Vec3f glow
                = lightColour(*makeGraphLight(osg::Vec4f(0, 0, 0, 0), osg::Vec4f(1.5f, 1.5f, 1.5f, 1)));
            EXPECT_NEAR(glow.x(), 2.53716f, 1e-4f);
            EXPECT_NEAR(glow.z(), 2.53716f, 1e-4f);

            // Both at once add as light adds, after each is decoded and not before: 0.21586 of red
            // on top of 2.53716 of white.
            const osg::Vec3f both = lightColour(*makeGraphLight(grey, osg::Vec4f(1.5f, 1.5f, 1.5f, 1)));
            EXPECT_NEAR(both.x(), 2.75302f, 1e-4f);
            EXPECT_NEAR(both.y(), 2.53716f, 1e-4f);

            // **The property the whole function exists for.** The harness reads a cell's `LIGH`
            // records and the game reads the `SceneUtil::LightSource` nodes its graph already holds;
            // for one record those two have to be one light, down to the last bit of the intensity.
            for (const std::uint32_t packed : { 0x00000000u, 0x00808080u, 0x000080FFu, 0x00FFFFFFu })
            {
                const ESM::Light record = makeRecord(100, packed, 0);
                const std::optional<Rtx::Light> fromRecord = makeLight(record, osg::Vec3f(1, 2, 3));

                const osg::ref_ptr<SceneUtil::LightSource> graph
                    = makeGraphLight(SceneUtil::colourFromRGB(packed), osg::Vec4f());
                const std::optional<Rtx::Light> fromGraph = makeLight(lightColour(*graph), 100.0f, osg::Vec3f(1, 2, 3));

                ASSERT_TRUE(fromRecord.has_value() && fromGraph.has_value()) << "packed " << packed;
                EXPECT_EQ(fromRecord->mIntensity, fromGraph->mIntensity) << "packed " << packed;
                EXPECT_EQ(fromRecord->mReach, fromGraph->mReach);
                EXPECT_EQ(fromRecord->mRadius, fromGraph->mRadius);
                EXPECT_EQ(fromRecord->mPosition, fromGraph->mPosition);
            }
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
        /// The hour holds an exposure back, and a noon does not.
        ///
        /// **The histogram cannot tell a midnight from a noon**, because it normalises whatever it
        /// is shown toward the key — so a renderer left to measure its own exposure has no night in
        /// it at any hour. The weather knows the hour absolutely, and this is what it says about it.
        TEST(RtxLightBuilderTest, theHourHoldsAnExposureBackAndANoonDoesNot)
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
        TEST(RtxLightBuilderTest, theAirTakesABodyOutAsItGoesDown)
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
        TEST(RtxLightBuilderTest, aNightsSkyLightsWithMoreThanItIsDrawnWith)
        {
            const auto grey = [](float value) { return osg::Vec3f(value, value, value); };
            const auto filled = [&](float horizon, float zenith, float sheets, float ambient) {
                return skyFill(grey(horizon), grey(zenith), grey(sheets), grey(ambient)).x();
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
            const osg::Vec3f tinted = skyFill(grey(0.5f), grey(0.5f), osg::Vec3f(), osg::Vec3f(0.9f, 0.5f, 0.1f));
            EXPECT_NEAR(tinted.x(), 0.4f, 1e-6f);
            EXPECT_EQ(tinted.y(), 0.0f);
            EXPECT_EQ(tinted.z(), 0.0f);
        }

        /// And the weather fills it, so a frame gets the hour it is at rather than a default.
        ///
        /// **The relation and not a number.** Which weather values this binary sees depends on
        /// whether a test before it opened the real installation, which
        /// `everyHourAsksOnlyForSettingsTheGameDefines` says more about. What holds either way is
        /// that the field is this hour's own light put through the curve rather than a default left
        /// standing.
        TEST(RtxLightBuilderTest, aDaylightCarriesTheHoursOwnBias)
        {
            for (const float hour : { 0.0f, 6.0f, 12.0f, 18.0f })
            {
                const Daylight day = makeDaylight("Clear", hour);
                EXPECT_FLOAT_EQ(day.mExposureBias, exposureBias(day.mSun.mIrradiance, day.mAmbient))
                    << "at hour " << hour;
            }
        }

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

        /// A light that subtracts is refused by both routes to one, and the graph was the half that
        /// was wrong.
        ///
        /// **`SceneUtil::createLightSource` has no notion of "not a light".** It answers a `Negative`
        /// record by negating the diffuse and handing back a `LightSource` like any other, so the
        /// walk mirrored a lamp of negative intensity exactly where the harness placed none. The
        /// refusal now lives where a colour and a radius meet, which is the one place both routes
        /// pass through — and this builds the graph the game builds rather than a negative colour by
        /// hand, so it is the real path that is refused.
        TEST(RtxLightBuilderTest, aLightThatSubtractsIsRefusedByBothRoutesToOne)
        {
            const ESM::Light subtracting = makeRecord(100, 0x00FFFFFF, ESM::Light::Negative);

            const osg::ref_ptr<SceneUtil::LightSource> built = SceneUtil::createLightSource(
                SceneUtil::LightCommon(subtracting), SceneUtil::Mask_Lighting, /*isExterior=*/false);
            ASSERT_NE(built, nullptr);

            const osg::Vec3f radiated = lightColour(*built);
            ASSERT_LT(radiated.x(), 0.0f) << "the graph did not build a light that subtracts, so this proves nothing";

            EXPECT_FALSE(makeLight(radiated, 100.0f, osg::Vec3f()).has_value()) << "the walk mirrored it anyway";
            EXPECT_FALSE(makeLight(subtracting, osg::Vec3f()).has_value()) << "and the record it was built from";

            // The same record without the flag is an ordinary white lamp by both routes, so what the
            // two agree on is the flag and not the light.
            const ESM::Light ordinary = makeRecord(100, 0x00FFFFFF, 0);
            const osg::ref_ptr<SceneUtil::LightSource> lit = SceneUtil::createLightSource(
                SceneUtil::LightCommon(ordinary), SceneUtil::Mask_Lighting, /*isExterior=*/false);

            EXPECT_TRUE(makeLight(lightColour(*lit), 100.0f, osg::Vec3f()).has_value());
            EXPECT_TRUE(makeLight(ordinary, osg::Vec3f()).has_value());

            // **A black record subtracts nothing, so the flag on it decides nothing either.** Both
            // routes place a lamp that radiates zero, which is what they already did for a black
            // record without the flag — the record path used to drop this one and disagree.
            const ESM::Light unlit = makeRecord(100, 0x00000000, ESM::Light::Negative);
            const osg::ref_ptr<SceneUtil::LightSource> dark = SceneUtil::createLightSource(
                SceneUtil::LightCommon(unlit), SceneUtil::Mask_Lighting, /*isExterior=*/false);

            EXPECT_TRUE(makeLight(lightColour(*dark), 100.0f, osg::Vec3f()).has_value());
            EXPECT_TRUE(makeLight(unlit, osg::Vec3f()).has_value());
        }
    }
}
