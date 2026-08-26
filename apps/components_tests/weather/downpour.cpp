#include <gtest/gtest.h>

#include <osg/Vec3f>

#include <components/fallback/fallback.hpp>
#include <components/settings/values.hpp>
#include <components/weather/downpour.hpp>

namespace Weather
{
    namespace
    {
        /// The two constants the game reads from its own store rather than the ini.
        constexpr float sStormWind = 50.0f;
        constexpr float sGravity = 500.0f;

        /// Seeds the handful of keys these tests read.
        ///
        /// **`Fallback::Map` keeps the first value a key is given**, and a test elsewhere in this
        /// binary opens the real installation — so which of the two any read below gets depends on
        /// the order the suite ran in, and what is asserted has to be true of either. These seeds
        /// are the shipped values for exactly that reason.
        void seed()
        {
            Fallback::Map::init({
                { "Weather_Clear_Using_Precip", "0" },
                { "Weather_Rain_Using_Precip", "1" },
                { "Weather_Thunderstorm_Using_Precip", "1" },
                { "Weather_Clear_Wind_Speed", "0.3" },
                { "Weather_Ashstorm_Wind_Speed", "0.8" },
            });
        }

        /// Which weathers drop rain is a fact about the content files, not about this code.
        ///
        /// **`Weather_<name>_Using_Precip` is the whole of the question** — Morrowind hard-codes the
        /// answer per weather and the engine draws a particle system rather than the mesh the
        /// setting names. Rain and thunder carry it, fair weather does not, and a harness that reads
        /// them the other way round would render a clear sky full of drops.
        TEST(WeatherDownpourTest, onlyTheWetWeathersCarryRain)
        {
            seed();

            EXPECT_TRUE(downpourAt("Clear", sStormWind, sGravity).mRainEffect.empty());
            EXPECT_FALSE(downpourAt("Rain", sStormWind, sGravity).mRainEffect.empty());
            EXPECT_FALSE(downpourAt("Thunderstorm", sStormWind, sGravity).mRainEffect.empty());
        }

        /// A storm is a wind speed over a threshold, and the threshold is the caller's.
        ///
        /// Proving the parameter matters rather than that the call returns: the same weather is a
        /// storm under one threshold and not under another, so the number reached the comparison.
        TEST(WeatherDownpourTest, aStormIsWhateverBlowsHarderThanTheThreshold)
        {
            seed();

            // Read rather than pinned, so the two thresholds below straddle whichever value won.
            const float ash = windSpeed("Ashstorm");
            ASSERT_GT(ash, 0.0f);

            EXPECT_TRUE(downpourAt("Ashstorm", ash * 0.5f, sGravity).mIsStorm);
            EXPECT_FALSE(downpourAt("Ashstorm", ash * 2.0f, sGravity).mIsStorm);
        }

        /// Every weather's rain falls at the gravity it was handed, and nothing else invents one.
        TEST(WeatherDownpourTest, rainFallsAtTheGravityItWasGiven)
        {
            EXPECT_FLOAT_EQ(downpourAt("Rain", sStormWind, sGravity).mRainSpeed, sGravity);
            EXPECT_FLOAT_EQ(downpourAt("Rain", sStormWind, 12.0f).mRainSpeed, 12.0f);
        }

        /// The wind that leans the drops is eight times the wind the files record, capped at
        /// seventy.
        ///
        /// **This is what made a shot of a rainstorm not look like one.** The harness drove the
        /// particles with the recorded number and the game drove them with this one, and
        /// `Precipitation::updateRainParameters` leans every drop by `atan(wind / 50)` — so the same
        /// weather fell at 0.34 degrees off vertical in a shot and 2.75 in the game.
        TEST(WeatherDownpourTest, theWindThatLeansTheDropsIsEightTimesTheRecordedOne)
        {
            // 8 * 0.3 = 2.4, which is under the cap.
            EXPECT_FLOAT_EQ(gustSpeed(0.3f), 2.4f);

            // 8 * 0.8 = 6.4, likewise.
            EXPECT_FLOAT_EQ(gustSpeed(0.8f), 6.4f);

            // 8 * 9 = 72, which is not: the cap is what a hurricane is allowed to reach.
            EXPECT_FLOAT_EQ(gustSpeed(9.0f), 70.0f);
            EXPECT_FLOAT_EQ(gustSpeed(8.75f), 70.0f);

            // Still, because nothing multiplied is still nothing.
            EXPECT_FLOAT_EQ(gustSpeed(0.0f), 0.0f);
        }

        /// A `Downpour` carries both winds, and they are not the same number.
        ///
        /// **Which is the whole reason there are two fields.** `mWindSpeed` leans the drops;
        /// `mBaseWindSpeed` is what the world is asked about — grass sway, a shader uniform — and
        /// answering either with the other is a visible mistake in one direction or the other.
        TEST(WeatherDownpourTest, aDownpourCarriesTheGustAndTheRecordedWindApart)
        {
            seed();

            const float recorded = windSpeed("Ashstorm");
            ASSERT_GT(recorded, 0.0f);

            const Downpour ash = downpourAt("Ashstorm", sStormWind, sGravity);

            EXPECT_FLOAT_EQ(ash.mBaseWindSpeed, recorded);
            EXPECT_FLOAT_EQ(ash.mWindSpeed, gustSpeed(recorded));
            EXPECT_GT(ash.mWindSpeed, ash.mBaseWindSpeed);

            // **And the threshold is read against the recorded one**, which is what the game does
            // when it builds a weather. Comparing the gust instead would make a storm of every
            // weather that blows at all.
            EXPECT_FALSE(downpourAt("Ashstorm", gustSpeed(recorded) * 0.5f, sGravity).mIsStorm);
        }

        /// An ash storm blows off Red Mountain at whoever is watching; everything else blows north.
        ///
        /// **Not a property of the weather alone**, which is why it is asked per frame with an
        /// observer rather than settled into the `Downpour` beside the rest of it.
        TEST(WeatherDownpourTest, onlyAshAndBlightAreAimedAtTheObserver)
        {
            const osg::Vec3f north = defaultStormDirection();

            // Due south of the mountain, so the storm drives due south — the y component is the
            // whole of the answer and it is the negative of north's.
            EXPECT_EQ(stormDirection(stormEffect("Ashstorm"), osg::Vec3f(25000.0f, 60000.0f, 0.0f)), -north);

            // Due east of it, and the height is ignored on the way.
            EXPECT_EQ(stormDirection(stormEffect("Blight"), osg::Vec3f(35000.0f, 70000.0f, 9000.0f)),
                osg::Vec3f(1.0f, 0.0f, 0.0f));

            // On the summit, where there is no direction away from it.
            EXPECT_EQ(stormDirection(stormEffect("Ashstorm"), osg::Vec3f(25000.0f, 70000.0f, 4000.0f)), north);

            // A blizzard drives a model too and is still not aimed at anybody.
            EXPECT_EQ(stormDirection(stormEffect("Blizzard"), osg::Vec3f(25000.0f, 60000.0f, 0.0f)), north);
            EXPECT_EQ(stormDirection(stormEffect("Rain"), osg::Vec3f(25000.0f, 60000.0f, 0.0f)), north);
        }

        /// Four weathers drive a model past the eye and the other six drive nothing.
        ///
        /// **The table the game's weather manager used to keep on its own.** Two copies of four
        /// entries is two chances to disagree about what a blizzard looks like.
        TEST(WeatherDownpourTest, fourWeathersDriveAModelAndTheRestDriveNone)
        {
            EXPECT_EQ(stormEffect("Ashstorm"), Settings::models().mWeatherashcloud.get());
            EXPECT_EQ(stormEffect("Blight"), Settings::models().mWeatherblightcloud.get());
            EXPECT_EQ(stormEffect("Snow"), Settings::models().mWeathersnow.get());
            EXPECT_EQ(stormEffect("Blizzard"), Settings::models().mWeatherblizzard.get());

            EXPECT_TRUE(stormEffect("Clear").empty());
            EXPECT_TRUE(stormEffect("Rain").empty());

            // Read out of a weather rather than beside it, because that is how `Precipitation` gets
            // it and a table consulted only by its own test proves nothing.
            EXPECT_EQ(downpourAt("Blizzard", sStormWind, sGravity).mParticleEffect,
                Settings::models().mWeatherblizzard.get());
        }
    }
}
