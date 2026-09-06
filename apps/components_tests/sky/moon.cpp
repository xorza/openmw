#include <array>
#include <cstddef>

#include <gtest/gtest.h>

#include <components/sky/moonmodel.hpp>

namespace Sky
{
    namespace
    {
        /// Masser's ten `Moons_Masser_*`, as the shipped fallbacks record them.
        ///
        /// **Built rather than read**, for the reason `sun.cpp`'s own fixture beside this gives: a
        /// test that reads `Fallback::Map` is reading whatever configuration the binary happens to
        /// have been planted with rather than its own premise.
        MoonModel masser()
        {
            return MoonModel(14.0f, 15.0f, 7.0f, 10.0f, 35.0f, 0.5f, 1.0f, 50.0f, 40.0f, 0.5f);
        }

        /// Secunda's, which differ in every number that separates the two arcs.
        MoonModel secunda()
        {
            return MoonModel(14.0f, 15.0f, 7.0f, 10.0f, 50.0f, 0.6f, 1.2f, 50.0f, 40.0f, 0.5f);
        }

        /// Degrees along its arc that Masser covers in an hour.
        ///
        /// **Not the 0.5 the ini asks for.** Fifteen degrees an hour is one rotation a day and the
        /// speed counts rotations, so 0.5 would leave the moon short of its own horizon in
        /// twenty-four hours; the engine floors every moon at `180 / 23 / 15` and Masser is the one
        /// that hits the floor. 15 * 0.5217391 is 7.826087 degrees an hour.
        constexpr float sMasserPerHour = 15.0f * (180.0f / 23.0f / 15.0f);

        /// Masser rises at four in the afternoon on the day the game begins, and climbs from there.
        ///
        /// The rise hour is `increment + (day - 1 + 16) * increment mod 24`, and Masser's increment
        /// is one, so day zero gives `1 + 15`.
        TEST(SkyMoonTest, masserRisesAtSixteenHundredOnTheDayTheGameBegins)
        {
            EXPECT_NEAR(masser().at(0, 16.0f).mAlongArc, 0.0f, 1e-4f) << "at the horizon it rises from";

            // Six hours later, and the arc is what the speed says and nothing else.
            EXPECT_NEAR(masser().at(0, 22.0f).mAlongArc, 6.0f * sMasserPerHour, 1e-3f);

            // **Zero again once it has set**, which is the engine's own way of saying it is not up
            // rather than a second flag beside the angle.
            EXPECT_EQ(masser().at(0, 15.0f).mAlongArc, 0.0f) << "an hour before it rises";
        }

        /// The arc is swung about the zenith, and the two moons are swung by different amounts.
        ///
        /// **Swinging and not tipping**, which is what makes both moons climb as high as the sun
        /// does and only their rising points differ — so their paths cross.
        TEST(SkyMoonTest, eachMoonsArcIsSwungByItsOwnOffset)
        {
            EXPECT_FLOAT_EQ(masser().at(0, 22.0f).mAxisOffset, 35.0f);
            EXPECT_FLOAT_EQ(secunda().at(0, 22.0f).mAxisOffset, 50.0f);

            // A faster moon on a longer day is somewhere else at the same hour, which is the whole
            // of why the two are read apart.
            EXPECT_NE(secunda().at(0, 22.0f).mAlongArc, masser().at(0, 22.0f).mAlongArc);
        }

        /// A moon fades out through the morning and back in through the afternoon.
        ///
        /// **`mDaylightFade` and not `mAlpha`.** The engine also hides a moon under
        /// `Fade_End_Angle`, which is a rasterizer keeping a lit quad off its own fogged horizon; a
        /// renderer that traces the air wants the hour's half of the answer without that.
        TEST(SkyMoonTest, theHourFadesAMoonOutThroughTheMorningAndBackInThroughTheAfternoon)
        {
            // Day nine is where Masser rises at one in the morning — `1 + (9 - 1 + 16) mod 24` — so
            // half past two in the afternoon finds it a hundred and six degrees along and inside the
            // hour-long fade in that runs from fourteen to fifteen. Half an hour of one hour is half
            // the moon.
            EXPECT_FLOAT_EQ(masser().at(9, 14.5f).mDaylightFade, 0.5f);
            EXPECT_FLOAT_EQ(masser().at(9, 15.0f).mDaylightFade, 1.0f) << "wholly back by the finish";

            // And between the fade out finishing and the fade in starting there is no moon at all,
            // whatever its arc says.
            EXPECT_EQ(masser().at(9, 12.0f).mDaylightFade, 0.0f);
            EXPECT_GT(masser().at(9, 8.5f).mDaylightFade, 0.0f) << "halfway out is not out";
        }

        /// The phase holds for three days and comes round in twenty-four.
        ///
        /// **Eight painted faces on a three-day cycle**, which is the engine's own `(counted / 3) %
        /// 8`: full on 16 Last Seed and waning from the seventeenth. `MoonPhase`'s doc says why the
        /// order of the eight matters and what else depends on it.
        ///
        /// **Read at a late hour on purpose.** A phase change is held back until the moon is out of
        /// the sky, so which of two days an hour belongs to depends on where that hour falls against
        /// `phaseHour` — and a test that read at noon would be asserting that boundary rather than
        /// the cycle.
        TEST(SkyMoonTest, thePhaseHoldsForThreeDaysAndComesRoundInTwentyFour)
        {
            const MoonModel clock = masser();
            const auto at = [&clock](const int day) { return clock.at(day, 22.0f).mPhase; };

            EXPECT_EQ(at(24), at(0)) << "twenty-four days is a cycle";

            // Every one of the eight is reached inside that cycle, so it is a walk over the faces
            // rather than a step between a few of them.
            std::array<bool, 8> seen{};
            for (int day = 0; day < 24; ++day)
                seen[static_cast<std::size_t>(at(day))] = true;

            for (std::size_t face = 0; face < seen.size(); ++face)
                EXPECT_TRUE(seen[face]) << "no day showed phase " << face;

            // And it holds rather than stepping: of any three days running, two share a face.
            for (int day = 0; day < 21; ++day)
                EXPECT_TRUE(at(day) == at(day + 1) || at(day + 1) == at(day + 2))
                    << "three days from " << day << " named three faces";
        }
    }
}
