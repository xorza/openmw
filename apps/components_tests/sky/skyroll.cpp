#include <cmath>

#include <gtest/gtest.h>

#include <components/sky/skyroll.hpp>

namespace Sky
{
    namespace
    {
        constexpr float sTau = 6.283185307179586f;

        /// The deck runs on the weather's own speed, and on whichever clock the content names.
        ///
        /// **Morrowind's arithmetic, and the reason it is not left in the sky manager.** A ray tracer
        /// draws the same deck and has no sky manager to ask, so a second copy of `speed / 400` is a
        /// second sky — one that would keep time with the first for exactly as long as nobody
        /// touched either.
        TEST(RtxSkyRollTest, theDeckScrollsAtTheWeathersSpeedAndOnTheClockTheContentNames)
        {
            // A second of a weather whose `Cloud_Speed` is Clear's 1.25: 1.25 / 400.
            SkyRoll real;
            real.advance(1.0f, 1.25f, 30.0f, false);
            EXPECT_FLOAT_EQ(real.mClouds, 1.25f / 400.0f);

            // **The same second on the world's clock instead**, which is what `Timescale_Clouds`
            // asks for: thirty game seconds to the real one, over the minute the engine divides by.
            SkyRoll scaled;
            scaled.advance(1.0f, 1.25f, 30.0f, true);
            EXPECT_FLOAT_EQ(scaled.mClouds, 1.25f / 400.0f * 0.5f);
            EXPECT_LT(scaled.mClouds, real.mClouds) << "and the two are not the same sky";

            // A still weather does not move at all, whichever clock it is on.
            SkyRoll windless;
            windless.advance(600.0f, 0.0f, 30.0f, true);
            EXPECT_FLOAT_EQ(windless.mClouds, 0.0f);
        }

        /// The scroll wraps into the range the engine's texture matrix runs over.
        ///
        /// **`fmod` and not one subtraction, which is what the engine does.** A frame long enough to
        /// scroll past four — a loading pause, or a harness stepping a run in one go — left its
        /// timer outside the range for as long as it took to come back round.
        TEST(RtxSkyRollTest, theScrollWrapsAtFourHoweverFarItWentInOneStep)
        {
            // Four exactly is nought again: `400 * 4 / 1.0` seconds at a speed of one.
            SkyRoll once;
            once.advance(1600.0f, 1.0f, 1.0f, false);
            EXPECT_NEAR(once.mClouds, 0.0f, 1e-4f);

            // Five is one, and it takes one step to say so rather than two.
            SkyRoll leapt;
            leapt.advance(2000.0f, 1.0f, 1.0f, false);
            EXPECT_NEAR(leapt.mClouds, 1.0f, 1e-4f);

            // Nine is one as well, which one subtraction could not have reached.
            SkyRoll further;
            further.advance(3600.0f, 1.0f, 1.0f, false);
            EXPECT_NEAR(further.mClouds, 1.0f, 1e-3f);

            for (int step = 0; step < 200; ++step)
            {
                further.advance(37.0f, 3.0f, 1.0f, false);
                EXPECT_GE(further.mClouds, 0.0f);
                EXPECT_LT(further.mClouds, 4.0f) << "at step " << step;
            }
        }

        /// The stars come round once every four days, and the weather has nothing to do with it.
        TEST(RtxSkyRollTest, theStarsComeRoundEveryFourDaysWhateverTheWeatherIs)
        {
            // Four days of ninety-six hours' worth of seconds, at a scale of one, is one turn — and
            // one turn is nought again.
            SkyRoll round;
            round.advance(3600.0f * 96.0f, 0.0f, 1.0f, false);
            EXPECT_NEAR(round.mStars, 0.0f, 1e-3f);

            // A quarter of it is a right angle, and a cloud speed of nothing did not stop it.
            SkyRoll quarter;
            quarter.advance(3600.0f * 24.0f, 0.0f, 1.0f, false);
            EXPECT_NEAR(quarter.mStars, 0.25f * sTau, 1e-3f);

            // And the world's clock always drives them, where the deck's only sometimes does.
            SkyRoll fast;
            fast.advance(3600.0f * 24.0f, 0.0f, 2.0f, false);
            EXPECT_NEAR(fast.mStars, 0.5f * sTau, 1e-3f);
        }
    }
}
