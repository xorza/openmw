#include <optional>

#include <gtest/gtest.h>

#include <components/rtx/reconstruction.hpp>
#include <components/rtx/renderer.hpp>

namespace Rtx
{
    namespace
    {
        /// **Every field the factory reads, set away from its default**, so that one it dropped
        /// would report `FrameOptions`' own default rather than the answer — which is exactly how
        /// `--filter`, `--jitter` and `--accumulate` were dead. The profile's other seven decide a
        /// device or a loader and have no field here to reach.
        TEST(RtxFrameOptionsTest, aFrameCarriesTheProfilesHalfAndItsOwn)
        {
            const RenderProfile asked{ .mReconstruction = { .mFilter = false, .mJitter = true },
                .mExposure = std::nullopt };
            const FrameOptions made = FrameOptions::forFrame(asked, 64, 0.02f, 1.5f);

            EXPECT_FALSE(made.mReconstruction.mFilter) << "the filter the profile turned off";
            EXPECT_TRUE(made.mReconstruction.mJitter) << "the jitter the profile turned on";
            EXPECT_EQ(made.mExposure, std::nullopt) << "the exposure the profile left to the frame";

            EXPECT_EQ(made.mAccumulate, 64u) << "what the schedule has averaged so far";
            ASSERT_TRUE(made.mSinceLast.has_value());
            EXPECT_FLOAT_EQ(*made.mSinceLast, 0.02f) << "the step the clock stated";
            EXPECT_FLOAT_EQ(made.mExposureBias, 1.5f) << "the bias the hour settled";
        }

        /// **A frame with no schedule behind it, which is every frame a player draws.** Nothing is
        /// averaged, the step is the renderer's own to measure, and the bias is one.
        ///
        /// **And the exposure is measured, which is the one field where the two types disagree on
        /// purpose.** `FrameOptions::mExposure` is one by default because a test constructing the
        /// struct wants a number it can hand-compute against; `RenderProfile::mExposure` is nothing
        /// by default because a played game wants the eye to adapt. Carrying the profile is what
        /// makes the run's answer win over the interface's.
        TEST(RtxFrameOptionsTest, aPlayedFrameAsksForNoneOfTheThreeAndMeasuresItsExposure)
        {
            const FrameOptions made = FrameOptions::forFrame(RenderProfile{}, 0, std::nullopt, 1.0f);

            EXPECT_EQ(made.mAccumulate, 0u);
            EXPECT_EQ(made.mSinceLast, std::nullopt);
            EXPECT_FLOAT_EQ(made.mExposureBias, 1.0f);

            EXPECT_TRUE(made.mReconstruction.mFilter) << "a profile that asked for nothing keeps the filter on";
            EXPECT_FALSE(made.mReconstruction.mJitter);
            EXPECT_EQ(made.mExposure, std::nullopt) << "a played frame measures the exposure off itself";
        }
    }
}
