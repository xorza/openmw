#include <gtest/gtest.h>

#include <components/rtx/renderer.hpp>

#include <apps/rtxtool/framerequest.hpp>
#include <apps/rtxtool/picture.hpp>
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

        /// Every request that stands a renderer up hands the choice on.
        ///
        /// **The bug this was written for: `doll` and `map` dropped it.** They built their options
        /// inline and named every field but the layers, so the two commands parsed
        /// `--sync-validation`, accepted it, and traced with nothing loaded — and a run under it came
        /// back clean because nothing was checking. The switch is now an argument of the conversion
        /// rather than a field of the request, so a caller has to have one in hand.
        TEST(RtxValidationChoiceTest, everyRequestHandsTheChoiceToTheRendererItDescribes)
        {
            const Rtx::ValidationOptions asked{
                .mEnabled = true, .mSynchronization = true, .mGpuAssisted = true, .mAbortOnError = true
            };

            PictureRequest picture;
            picture.mWidth = 512;
            picture.mHeight = 1024;

            const Rtx::RendererOptions drawn = picture.describeRenderer(asked);
            EXPECT_TRUE(drawn.mValidation.mEnabled);
            EXPECT_TRUE(drawn.mValidation.mSynchronization);
            EXPECT_TRUE(drawn.mValidation.mGpuAssisted);
            EXPECT_TRUE(drawn.mValidation.mAbortOnError) << "the whole choice and not the layers alone";
            EXPECT_EQ(drawn.mWidth, 512u) << "and the rest of the picture came with it";
            EXPECT_EQ(drawn.mHeight, 1024u);

            FrameRequest frame;
            frame.mWidth = 1920;
            frame.mHeight = 1080;

            const Rtx::RendererOptions traced = frame.describeRenderer(asked);
            EXPECT_TRUE(traced.mValidation.mEnabled);
            EXPECT_TRUE(traced.mValidation.mSynchronization);
            EXPECT_TRUE(traced.mValidation.mGpuAssisted);
            EXPECT_TRUE(traced.mValidation.mAbortOnError);
            EXPECT_EQ(traced.mWidth, 1920u);
            EXPECT_EQ(traced.mHeight, 1080u);
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
