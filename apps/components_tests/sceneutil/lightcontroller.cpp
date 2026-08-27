#include <components/sceneutil/lightcontroller.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
    using LightType = SceneUtil::LightController::LightType;

    /// One light's animation, sampled.
    struct Lamp
    {
        SceneUtil::LightController mController;

        explicit Lamp(LightType type) { mController.setType(type); }

        float at(double seconds) const { return mController.brightnessAt(seconds); }

        /// `count` samples, `step` seconds apart, from zero.
        std::vector<float> run(std::size_t count, double step) const
        {
            std::vector<float> out;
            out.reserve(count);

            for (std::size_t i = 0; i < count; ++i)
                out.push_back(at(static_cast<double>(i) * step));

            return out;
        }
    };

    /// How often the light crosses its own resting brightness, per second.
    float crossingsPerSecond(const std::vector<float>& run, double step)
    {
        std::size_t crossings = 0;
        for (std::size_t i = 1; i < run.size(); ++i)
            if ((run[i] - 1.0f) * (run[i - 1] - 1.0f) < 0.0f)
                ++crossings;

        return static_cast<float>(static_cast<double>(crossings) / (static_cast<double>(run.size() - 1) * step));
    }

    float mean(const std::vector<float>& run)
    {
        double total = 0.0;
        for (const float value : run)
            total += value;

        return static_cast<float>(total / static_cast<double>(run.size()));
    }

    /// A light the record says nothing about burns at exactly what it is.
    TEST(SceneUtilLightControllerTest, aSteadyLightIsExactlyOne)
    {
        const Lamp steady(SceneUtil::LightController::LT_Normal);

        for (const double seconds : { 0.0, 0.017, 3.5, 1e5 })
            EXPECT_EQ(steady.at(seconds), 1.0f);
    }

    /// A flame stays inside its depth and, over time, radiates exactly what the record says.
    ///
    /// **The mean is the point.** The model this replaced walked toward a random target between a
    /// quarter and one, so a flickering light averaged 0.63 of its recorded colour — while the
    /// harness, which runs no update traversal at all, drew the same lamp at 1.0. The two disagreed
    /// by three fifths of a stop about every candle in the game.
    TEST(SceneUtilLightControllerTest, aFlameStaysWithinItsDepthAndAveragesOne)
    {
        for (const LightType type :
            { SceneUtil::LightController::LT_Flicker, SceneUtil::LightController::LT_FlickerSlow,
                SceneUtil::LightController::LT_Pulse, SceneUtil::LightController::LT_PulseSlow })
        {
            const Lamp lamp(type);
            const std::vector<float> run = lamp.run(60000, 0.01);
            const bool pulse
                = type == SceneUtil::LightController::LT_Pulse || type == SceneUtil::LightController::LT_PulseSlow;

            // The bands are weighted to sum to one, so the depth is a bound and not a statistic.
            const float depth = pulse ? 0.35f : 0.30f;
            EXPECT_GE(*std::min_element(run.begin(), run.end()), 1.0f - depth);
            EXPECT_LE(*std::max_element(run.begin(), run.end()), 1.0f + depth);

            // Ten minutes is at least a hundred turns of the slowest band any of them carries, so
            // what is left of it here is a thousandth.
            EXPECT_NEAR(mean(run), 1.0f, 0.001f);

            // And it did move, rather than sitting at its mean and passing the two tests above.
            EXPECT_GT(*std::max_element(run.begin(), run.end()) - *std::min_element(run.begin(), run.end()), depth);
        }
    }

    /// The fast flicker is the flame itself and the slow one is that flame seen through glass.
    ///
    /// Both are four bands of one ladder; the slow one takes its window a step down, so it loses the
    /// puffing at the top and gains a drift at the bottom. One step of the ladder is 2.618, and the
    /// rate at which the light crosses its own mean follows it: about 11 times a second against
    /// about 4.
    TEST(SceneUtilLightControllerTest, theSlowFlickerIsTheSameFlameOneStepDownTheLadder)
    {
        const Lamp fast(SceneUtil::LightController::LT_Flicker);
        const Lamp slow(SceneUtil::LightController::LT_FlickerSlow);

        // 200 hertz, so the nine-hertz band's own crossings are resolved rather than counted twice.
        const float busy = crossingsPerSecond(fast.run(12000, 0.005), 0.005);
        const float gentle = crossingsPerSecond(slow.run(12000, 0.005), 0.005);

        EXPECT_GT(busy, 8.0f);
        EXPECT_LT(gentle, 6.0f);
        EXPECT_GT(busy, gentle * 2.0f) << "the two flicker flags read as the same light";
    }

    /// A pulse is one sine, so it comes back to where it was and its two halves cancel exactly.
    ///
    /// The slow one turns once every three seconds and the fast one is a step of the ladder above
    /// it, at 3 / 2.618 = 1.1459 seconds.
    TEST(SceneUtilLightControllerTest, aPulseIsExactlyPeriodic)
    {
        const Lamp slow(SceneUtil::LightController::LT_PulseSlow);

        for (const double seconds : { 0.0, 0.3, 1.7, 10.5, 123.25 })
        {
            EXPECT_NEAR(slow.at(seconds), slow.at(seconds + 3.0), 1e-5f);

            // Half a turn on, the sine is its own negative, so the pair averages the resting
            // brightness whatever phase this lamp was given.
            EXPECT_NEAR(slow.at(seconds) + slow.at(seconds + 1.5), 2.0f, 1e-5f);
        }

        const Lamp fast(SceneUtil::LightController::LT_Pulse);
        constexpr double period = 3.0 / 2.618034;

        for (const double seconds : { 0.0, 0.3, 1.7, 10.5 })
            EXPECT_NEAR(fast.at(seconds), fast.at(seconds + period), 1e-5f);
    }

    /// The clock is the whole of the state, so the same instant always gives the same light.
    ///
    /// **What this buys is that anyone may ask.** The rasterizer's update traversal, the ray
    /// tracer's own walk and a test all reach the same answer for a frame, at any frame rate, in any
    /// order, and however many times — which is what lets the light be computed where it is read
    /// instead of being written once by whoever got there first.
    TEST(SceneUtilLightControllerTest, theSameInstantAlwaysGivesTheSameBrightness)
    {
        const Lamp lamp(SceneUtil::LightController::LT_FlickerSlow);
        const std::vector<double> scrambled = { 4.5, 0.25, 91.0, 4.5, 0.25, 17.75, 91.0 };

        std::vector<float> first;
        for (const double seconds : scrambled)
            first.push_back(lamp.at(seconds));

        for (std::size_t i = 0; i < scrambled.size(); ++i)
            EXPECT_EQ(lamp.at(scrambled[i]), first[i]) << "at " << scrambled[i];

        EXPECT_EQ(first[0], first[3]);
        EXPECT_EQ(first[2], first[6]);
    }

    /// Two candles standing together do not flicker together.
    ///
    /// Every light draws its own phase when it is built, and that is the only thing separating one
    /// from another. Measured across sixty-four of them rather than between two, so that the test
    /// does not rest on two draws being far apart.
    TEST(SceneUtilLightControllerTest, lampsBuiltTogetherStillFlickerApart)
    {
        std::vector<float> lit;
        for (int i = 0; i < 64; ++i)
            lit.push_back(Lamp(SceneUtil::LightController::LT_PulseSlow).at(0.0));

        double total = 0.0;
        for (const float value : lit)
            total += value;

        const double average = total / static_cast<double>(lit.size());
        double spread = 0.0;
        for (const float value : lit)
            spread += (value - average) * (value - average);

        // A pulse read at one instant across uniform phases has a deviation of 0.35 / sqrt(2), which
        // is 0.247. Half of that is far below anything sixty-four draws reach by chance and far
        // above the nothing a shared phase would give.
        EXPECT_GT(std::sqrt(spread / static_cast<double>(lit.size())), 0.12);
    }
}
