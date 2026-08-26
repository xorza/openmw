#include <gtest/gtest.h>

#include <components/rtx/renderer.hpp>

#include <apps/rtxtool/validationchoice.hpp>

namespace RtxTool
{
    namespace
    {
        /// What the three switches look like when nobody has said anything, outside a Release build.
        constexpr CommandSwitch sDefaultOn{ .mValue = true, .mGiven = false };
        constexpr CommandSwitch sAsked{ .mValue = true, .mGiven = true };
        constexpr CommandSwitch sRefused{ .mValue = false, .mGiven = true };

        /// Left alone, a development build validates everything a headless run can.
        TEST(RtxValidationChoiceTest, theDefaultsLoadEveryLayerAWindowCanCarry)
        {
            const Rtx::ValidationOptions headless = chooseValidation(sDefaultOn, sDefaultOn, sDefaultOn, false);
            EXPECT_TRUE(headless.mEnabled);
            EXPECT_TRUE(headless.mSynchronization);
            EXPECT_TRUE(headless.mGpuAssisted);

            // A window is the one place the GPU-assisted layer cannot be left on, and only where it
            // was not asked for by name.
            EXPECT_FALSE(chooseValidation(sDefaultOn, sDefaultOn, sDefaultOn, true).mGpuAssisted);
            EXPECT_TRUE(chooseValidation(sDefaultOn, sDefaultOn, sAsked, true).mGpuAssisted);
        }

        /// The bug this rule was written for: refusing the layers has to turn them off.
        ///
        /// Both finer switches imply the layers and both default on, so leaving their defaults
        /// standing meant `--validation=false` changed nothing at all — and the tool told anyone
        /// timing a frame to pass exactly that.
        TEST(RtxValidationChoiceTest, refusingTheLayersTurnsOffWhatWasOnlyOnByDefault)
        {
            const Rtx::ValidationOptions off = chooseValidation(sRefused, sDefaultOn, sDefaultOn, false);
            EXPECT_FALSE(off.mEnabled);
            EXPECT_FALSE(off.mSynchronization);
            EXPECT_FALSE(off.mGpuAssisted);
        }

        /// A switch asked for by name beats a blanket refusal, which is the more specific request
        /// winning rather than the later one.
        TEST(RtxValidationChoiceTest, aSwitchAskedForByNameSurvivesARefusalOfTheRest)
        {
            const Rtx::ValidationOptions sync = chooseValidation(sRefused, sAsked, sDefaultOn, false);
            EXPECT_TRUE(sync.mSynchronization);
            EXPECT_FALSE(sync.mGpuAssisted) << "still only on by default, and still refused";
            EXPECT_TRUE(sync.mEnabled) << "synchronization validation implies the layer that carries it";

            const Rtx::ValidationOptions gpu = chooseValidation(sRefused, sDefaultOn, sAsked, false);
            EXPECT_TRUE(gpu.mGpuAssisted);
            EXPECT_FALSE(gpu.mSynchronization);
            EXPECT_TRUE(gpu.mEnabled);
        }

        /// Nothing on by default, which is a Release build, and each switch still reaches its layer.
        TEST(RtxValidationChoiceTest, aReleaseBuildStaysQuietUntilSomethingIsAskedFor)
        {
            constexpr CommandSwitch quiet{ .mValue = false, .mGiven = false };

            EXPECT_FALSE(chooseValidation(quiet, quiet, quiet, false).mEnabled);
            EXPECT_TRUE(chooseValidation(sAsked, quiet, quiet, false).mEnabled);
            EXPECT_TRUE(chooseValidation(quiet, sAsked, quiet, false).mEnabled);
            EXPECT_TRUE(chooseValidation(quiet, quiet, sAsked, false).mEnabled);
        }
    }
}
