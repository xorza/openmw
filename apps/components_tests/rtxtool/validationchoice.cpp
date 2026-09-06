#include <gtest/gtest.h>

#include <apps/rtxtool/framerequest.hpp>
#include <apps/rtxtool/validationchoice.hpp>
#include <components/rtx/renderer.hpp>

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
        TEST(RtxValidationChoiceTest, aReleaseBuildStaysQuietUntilSomethingIsAskedFor)
        {
            constexpr CommandSwitch quiet{ .mValue = false, .mGiven = false };

            EXPECT_FALSE(chooseValidation(quiet, quiet, quiet, false).mEnabled);
            EXPECT_TRUE(chooseValidation(sAsked, quiet, quiet, false).mEnabled);
            EXPECT_TRUE(chooseValidation(quiet, sAsked, quiet, false).mEnabled);
            EXPECT_TRUE(chooseValidation(quiet, quiet, sAsked, false).mEnabled);
        }

        /// A run that named a layer demands it; a build that switched one on does not.
        ///
        /// **The difference decides whether a missing layer stops the run.** Without the layers
        /// nothing reports, so a gate that asked for them and got none reads an empty log as a pass
        /// — while a developer whose build turned them on by default still wants a renderer that
        /// starts. `Rtx::ValidationOptions::mDemanded` is what tells the two apart.
        TEST(RtxValidationChoiceTest, onlyASwitchNamedOnTheCommandLineDemandsTheLayers)
        {
            constexpr CommandSwitch quiet{ .mValue = false, .mGiven = false };

            EXPECT_FALSE(chooseValidation(sDefaultOn, sDefaultOn, sDefaultOn, false).mDemanded)
                << "a build default demanded the layers";
            EXPECT_FALSE(chooseValidation(sRefused, quiet, quiet, false).mDemanded)
                << "turning the layers down demanded them";

            EXPECT_TRUE(chooseValidation(sAsked, quiet, quiet, false).mDemanded);
            EXPECT_TRUE(chooseValidation(quiet, sAsked, quiet, false).mDemanded);
            EXPECT_TRUE(chooseValidation(quiet, quiet, sAsked, false).mDemanded);

            // A demand for the finer layer stands even against a refusal of the coarser one, which
            // is the same rule `mEnabled` follows above.
            EXPECT_TRUE(chooseValidation(sRefused, sAsked, quiet, false).mDemanded);
        }
    }
}
