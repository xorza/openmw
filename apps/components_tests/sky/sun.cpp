#include <cmath>

#include <gtest/gtest.h>

#include <components/sky/sun.hpp>

namespace Sky
{
    namespace
    {
        /// Morrowind's own hours, built rather than read.
        ///
        /// **Nothing here touches `Fallback::Map`.** It is one global map for the whole binary that
        /// keeps the first value written to a key, so a test that seeded it would decide what every
        /// later test in the file read — and would itself read whatever an earlier one had already
        /// put there. These four boundaries and one window are the whole of what the disc needs.
        TimeOfDaySettings vanillaHours()
        {
            TimeOfDaySettings times{};
            times.mNightEnd = 6.0f;
            times.mDayStart = 8.0f;
            times.mDayEnd = 18.0f;
            times.mNightStart = 20.0f;

            // `Weather_Sun_Pre-Sunset_Time` and its three siblings, as the shipped fallbacks record
            // them. Only the pre-sunset one reaches the disc's colour; the rest are here so this
            // reads as the setting the game assembles rather than as one field of it.
            times.mSunriseTransitions["Sun"] = WeatherSetting{ 0.0f, 0.0f, 1.0f, 1.25f };

            return times;
        }

        /// Clear's own disc tint, out of the content files: `255, 189, 157`.
        const osg::Vec4f sSunsetTint(1.0f, 189.0f / 255.0f, 157.0f / 255.0f, 1.0f);

        /// A black ambient, so the engine's add-and-clamp step leaves the lerp alone and the
        /// numbers below stay the ones a person can check by hand.
        const osg::Vec4f sNoAmbient(0.0f, 0.0f, 0.0f, 1.0f);

        /// The disc rises out of the horizon and sets back into it, and the light never does.
        ///
        /// **Two vectors from one parameter, and using the wrong one is what a sun that never sets
        /// looks like.** Morrowind gives the light a fixed northing and climb — so it hangs at
        /// fourteen degrees and swings — and gives the disc a height of `swing - |east|`, which is
        /// nought at either end of the day. Drawing the disc back along the light is why it hung
        /// there and pendulumed instead of setting.
        TEST(RtxSunArcTest, theDiscTouchesTheHorizonAndTheLightNeverDoes)
        {
            const TimeOfDaySettings times = vanillaHours();
            const float sunrise = times.mNightEnd;
            const float nightStart = times.mNightStart;

            // Level with the horizon at both ends of the day, to the bit: `400 - |400|`.
            EXPECT_NEAR(sunAt(sunrise, times).mPosition.z(), 0.0f, 1e-6f) << "sunrise";
            EXPECT_NEAR(sunAt(nightStart, times).mPosition.z(), 0.0f, 1e-6f) << "sunset";

            // And high at noon: `400` against a northing of 75, which is 79.4 degrees up.
            const osg::Vec3f noon = sunAt(13.0f, times).mPosition;
            EXPECT_NEAR(osg::RadiansToDegrees(std::asin(noon.z())), 79.4f, 0.1f);

            // **The two are not each other's negation**, which is the whole point of carrying both.
            const Sky::SunPlacement dawn = sunAt(sunrise, times);
            EXPECT_GT((dawn.mPosition + dawn.mDirection).length(), 0.1f);

            // The light keeps its fixed climb all day, which is what makes it never set.
            for (const float hour : { 6.0f, 9.0f, 13.0f, 17.0f, 20.0f })
            {
                const osg::Vec3f light = sunAt(hour, times).mDirection;
                EXPECT_NEAR(light.z() / light.y(), -100.0f / 75.0f, 1e-5f) << "at hour " << hour;
            }

            // **Night and `mShare` are two answers on purpose, and they differ at the boundaries.**
            // The skybox changes at an instant where the sun fades across an hour, so at exactly
            // sunrise there is no sun (`mShare` is nought) and it is nonetheless not night.
            EXPECT_FALSE(sunAt(13.0f, times).mNight);
            EXPECT_TRUE(sunAt(22.0f, times).mNight);
            EXPECT_TRUE(sunAt(3.0f, times).mNight) << "and the small hours are the same night";

            EXPECT_FALSE(sunAt(sunrise, times).mNight) << "sunrise is not night";
            EXPECT_FLOAT_EQ(sunAt(sunrise, times).mShare, 0.0f) << "and there is still no sun in it";
        }

        /// the two are mirror images. Its length is 125 at the midpoint, which makes that one exact.
        TEST(RtxSunArcTest, theSunCrossesTheSkyTheWayTheEngineSaysItDoes)
        {
            const TimeOfDaySettings times = vanillaHours();
            const float sunrise = times.mNightEnd;
            const float nightStart = times.mNightStart;

            // Halfway through a fourteen-hour day is hour 13, where the orbit is zero and the vector
            // is (0, 75, -100) — length exactly 125, so the direction is exactly (0, 0.6, -0.8).
            const osg::Vec3f noon = sunAt(13.0f, times).mDirection;
            EXPECT_NEAR(noon.x(), 0.0f, 1e-6f);
            EXPECT_NEAR(noon.y(), 0.6f, 1e-6f);
            EXPECT_NEAR(noon.z(), -0.8f, 1e-6f);

            // At either end the swing is full: (-+400, 75, -100), whose length is 419.077.
            const osg::Vec3f dawn = sunAt(sunrise, times).mDirection;
            const osg::Vec3f dusk = sunAt(nightStart, times).mDirection;

            EXPECT_NEAR(dawn.x(), -400.0f / 419.077f, 1e-4f) << "light travels west at dawn";
            EXPECT_NEAR(dusk.x(), 400.0f / 419.077f, 1e-4f) << "and east at dusk";
            EXPECT_NEAR(dawn.x(), -dusk.x(), 1e-6f) << "the two ends mirror";

            // **Through the daylit half the northing and the climb are fixed**, so their ratio is
            // the same at every hour of it: the day reproduces the engine's own vector exactly, and
            // only the night departs from it.
            for (const float hour : { 6.0f, 9.0f, 13.0f, 17.0f, 20.0f })
            {
                const osg::Vec3f at = sunAt(hour, times).mDirection;
                EXPECT_NEAR(at.z() / at.y(), -100.0f / 75.0f, 1e-5f) << "at hour " << hour;
                EXPECT_LT(at.z(), 0.0f) << "the light travels downward while the sun is up, at " << hour;
            }
        }

        /// The sun crosses the sky without ever jumping, which is the whole of the arc.
        ///
        /// **A seam here is visible as a sun that leaps across the horizon** when the clock is
        /// stepped over a boundary — which is what happened while the night was decided by a phase
        /// function that padded sunrise and sunset by an hour while the arc itself was measured from
        /// the unpadded times. Between the padding and the boundary the day's arc ran past its own
        /// end, and stepping back and forth over it ping-ponged the sun.
        ///
        /// The engine avoids it by shifting both times into a window that begins at sunrise
        /// (`apps/openmw/mwworld/weather.cpp:583`), so the two arcs meet exactly.
        TEST(RtxSunArcTest, theSunCrossesWithoutASeamAtEitherBoundary)
        {
            const TimeOfDaySettings times = vanillaHours();
            const float sunrise = times.mNightEnd;
            const float nightStart = times.mNightStart;

            // At the two boundaries the day's arc and the night's have to agree, or the sky has a
            // step in it: due west at dusk and due east at dawn.
            const osg::Vec3f dusk = sunAt(nightStart, times).mDirection;
            const osg::Vec3f dawn = sunAt(sunrise, times).mDirection;
            EXPECT_NEAR(dusk.x(), -dawn.x(), 1e-5f) << "the two horizons are opposite";

            // **Bounded by the fastest the arc ever moves, which is at the zenith.** The orbit is
            // linear in the hour and the direction is `(-400 orbit, 75, -100)` normalised, so the
            // angular rate peaks where the vector is shortest — 125 units at the crossing. The night
            // covers two of orbit in ten hours, a twentieth of an hour of which moves the tip four
            // units of four hundred: a chord of about 0.032. Anything past a twentieth is a seam,
            // and the one this is here for jumped by more than a whole unit vector.
            osg::Vec3f previous = sunAt(0.0f, times).mDirection;
            for (float hour = 0.05f; hour < 24.0f; hour += 0.05f)
            {
                const osg::Vec3f at = sunAt(hour, times).mDirection;
                EXPECT_LT((at - previous).length(), 0.05f) << "the sun jumped at hour " << hour;
                previous = at;
            }

            // And across the wrap, which is the third boundary and the one nothing else looks at.
            EXPECT_LT((sunAt(0.0f, times).mDirection - sunAt(23.95f, times).mDirection).length(), 0.05f)
                << "the sun jumped at midnight";

            // Both joins meet exactly, which is what "the two arcs are one circle" means.
            EXPECT_LT(
                (sunAt(nightStart - 1e-3f, times).mDirection - sunAt(nightStart + 1e-3f, times).mDirection).length(),
                1e-3f)
                << "a seam at dusk";
            EXPECT_LT(
                (sunAt(sunrise - 1e-3f, times).mDirection - sunAt(sunrise + 1e-3f, times).mDirection).length(), 1e-3f)
                << "a seam at dawn";
        }

        /// The disc's own colour is white all day, and the sunlight's is not.
        ///
        /// **The bug this is here for is a blue sun.** `Sun_*_Color` crosses to a night value of
        /// `59, 97, 176` across the whole sunrise window, and the disc comes up in the middle of
        /// that — so a renderer that painted the disc with the light drew a blue one every dawn.
        TEST(RtxSunDiscTest, theDiscIsWhiteAllDayAndWarmOnlyOnTheWayDown)
        {
            const TimeOfDaySettings times = vanillaHours();
            const float preSunset = times.getSetting("Sun").mPreSunsetTime;
            const float turns = times.mDayEnd - preSunset;
            ASSERT_GT(preSunset, 0.0f) << "a window with no width has no midpoint to check";

            // **White for the whole of the day**, including every hour of the sunrise the light
            // spends crossing out of its night blue.
            for (const float hour : { times.mNightEnd + 0.25f, times.mDayStart, 12.0f, turns - 0.01f })
            {
                const osg::Vec3f disc = sunDiscAt(hour, times, sSunsetTint, sNoAmbient);
                EXPECT_FLOAT_EQ(disc.x(), 1.0f) << "at hour " << hour;
                EXPECT_FLOAT_EQ(disc.y(), 1.0f) << "at hour " << hour;
                EXPECT_FLOAT_EQ(disc.z(), 1.0f) << "at hour " << hour;
            }

            // Halfway down the window, so halfway from white to the recorded tint: green is
            // (1 + 189/255) / 2 = 0.870588, blue (1 + 157/255) / 2 = 0.807843.
            const osg::Vec3f dusk = sunDiscAt(turns + 0.5f * preSunset, times, sSunsetTint, sNoAmbient);
            EXPECT_FLOAT_EQ(dusk.x(), 1.0f);
            EXPECT_NEAR(dusk.y(), 0.870588f, 1e-4f);
            EXPECT_NEAR(dusk.z(), 0.807843f, 1e-4f);

            // And all of it by the end of the window, which is sunset itself.
            const osg::Vec3f down = sunDiscAt(times.mDayEnd, times, sSunsetTint, sNoAmbient);
            EXPECT_NEAR(down.y(), 189.0f / 255.0f, 1e-4f);
            EXPECT_NEAR(down.z(), 157.0f / 255.0f, 1e-4f);

            // **The ambient is added and the sum clamped, which is Morrowind's mistake kept.** A
            // grey ambient of a fifth lifts each channel by a fifth of itself, and only the ones
            // that do not clip move: red is already one and stays there, so the sunset reads a
            // different hue from the one the file records rather than a brighter version of it.
            const osg::Vec3f lifted = sunDiscAt(times.mDayEnd, times, sSunsetTint, osg::Vec4f(0.2f, 0.2f, 0.2f, 1.0f));
            EXPECT_FLOAT_EQ(lifted.x(), 1.0f) << "clipped";
            EXPECT_NEAR(lifted.y(), 1.2f * 189.0f / 255.0f, 1e-4f);
            EXPECT_NEAR(lifted.z(), 1.2f * 157.0f / 255.0f, 1e-4f);
            EXPECT_GT(lifted.y(), down.y()) << "and so it is not the recorded colour";
        }

        /// How much of the sun there is, which is one number and the only one.
        ///
        /// **The bug this is here for is a shadow with no sun.** The engine states this twice — a
        /// disc alpha that runs on through the night and comes back at one, and a separate switch
        /// saying whether the sprite is drawn — because a rasterizer that has hidden the sprite has
        /// no use for the other answer. A tracer asks it to decide whether to cast, so the two
        /// halves are folded here: what is nought lights nothing, casts nothing and draws nothing.
        TEST(RtxSunShareTest, thereIsNoSunAtNightAndItLandsOnTheHorizonAtEitherEnd)
        {
            const TimeOfDaySettings times = vanillaHours();
            const float sunrise = 0.5f * (times.mDayStart - times.mNightEnd);
            const float dusk = times.mNightStart - times.mDayEnd;
            ASSERT_GT(sunrise, 0.0f);
            ASSERT_GT(dusk, 0.0f);

            // The two hours `sunAt` puts the disc level with the horizon, and there is none of it at
            // either — so it lands on the horizon rather than being switched off above one.
            EXPECT_FLOAT_EQ(sunShareAt(times.mNightEnd, times), 0.0f) << "sunrise";
            EXPECT_FLOAT_EQ(sunShareAt(times.mNightStart, times), 0.0f) << "sunset";

            // **And nothing anywhere in between them the other way round**, which is the half the
            // engine keeps elsewhere: its own curve returns one at every one of these hours.
            for (const float hour : { 0.0f, 2.0f, 5.0f, 20.5f, 22.0f, 23.9f })
                EXPECT_FLOAT_EQ(sunShareAt(hour, times), 0.0f) << "at hour " << hour;

            // Linear in over the first half of the sunrise window, and it is the hour past dawn
            // rather than a fraction of anything — which is why Morrowind's two-hour sunrise arrives
            // at exactly one at the end of it.
            EXPECT_FLOAT_EQ(sunShareAt(times.mNightEnd + 0.5f * sunrise, times), 0.5f * sunrise);
            EXPECT_FLOAT_EQ(sunShareAt(times.mNightEnd + sunrise, times), sunrise);
            EXPECT_FLOAT_EQ(sunShareAt(12.0f, times), 1.0f) << "and all of it through the day";

            // **Squared on the way out**, so the sun holds most of itself and then goes quickly:
            // halfway through dusk it is still three quarters there, `1 - 0.5^2`.
            EXPECT_FLOAT_EQ(sunShareAt(times.mDayEnd + 0.5f * dusk, times), 0.75f);
            EXPECT_LT(sunShareAt(times.mDayEnd + 0.75f * dusk, times), 0.45f);

            // Never past one, whatever a file says the sunrise is worth. The engine's dawn ramp is
            // unbounded and it did not matter while it was only an alpha; it scales the sunlight now.
            TimeOfDaySettings slow = times;
            slow.mDayStart = slow.mNightEnd + 8.0f;
            for (float hour = slow.mNightEnd; hour < slow.mNightStart; hour += 0.25f)
                EXPECT_LE(sunShareAt(hour, slow), 1.0f) << "at hour " << hour;

            // And it moves without a step anywhere, which is what stops the shadows jumping when the
            // sun goes out: the two curves and the night all meet at nought.
            float previous = sunShareAt(0.0f, times);
            for (float hour = 0.01f; hour < 24.0f; hour += 0.01f)
            {
                const float at = sunShareAt(hour, times);
                EXPECT_LT(std::abs(at - previous), 0.02f) << "the sun stepped at hour " << hour;
                previous = at;
            }
        }
    }
}
