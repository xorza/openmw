#include <gtest/gtest.h>

#include <components/rtx/reconstruction.hpp>

namespace Rtx
{
    namespace
    {
        /// The rule that used to be two expressions in the middle of the frame path.
        ///
        /// **Worth a test of its own because it is a function of its inputs and nothing else** — no
        /// device, no scene, no frame. What it decides used to be decided where nothing could ask
        /// about it, which is the whole reason it moved.
        TEST(RtxReconstructionTest, anUpscalerDenoisesForItselfAndJittersWhateverWasAsked)
        {
            // Nothing upscaling: the two switches mean exactly what they say.
            const Reconstruction wavelet
                = Reconstruction::resolve(Upscale::Off, ReconstructionRequest{ .mFilter = true, .mJitter = false });
            EXPECT_EQ(wavelet.mDenoiser, Denoiser::Wavelet);
            EXPECT_FALSE(wavelet.mJitter);
            EXPECT_FALSE(wavelet.mFilterSuppressed) << "nothing overruled it";
            EXPECT_FALSE(wavelet.mJitterForced);

            const Reconstruction raw
                = Reconstruction::resolve(Upscale::Off, ReconstructionRequest{ .mFilter = false, .mJitter = true });
            EXPECT_EQ(raw.mDenoiser, Denoiser::None) << "which is what a converged reference is built from";
            EXPECT_TRUE(raw.mJitter) << "and jitter is what makes that reference antialiased";

            // **The same request, and an upscaler in the way of it.** Both switches stop deciding:
            // the wavelet does not run because Ray Reconstruction is itself the denoiser, and the
            // frame jitters because reconstruction across frames of one sample point is
            // reconstruction from one sample. Neither of those is new behaviour; what is new is that
            // the answer says both happened.
            const Reconstruction upscaled = Reconstruction::resolve(
                Upscale::Quality, ReconstructionRequest{ .mFilter = true, .mJitter = false, .mPreset = Preset::E });
            EXPECT_EQ(upscaled.mDenoiser, Denoiser::RayReconstruction);
            EXPECT_NE(upscaled.mDenoiser, wavelet.mDenoiser) << "the same request, a different denoiser";
            EXPECT_TRUE(upscaled.mJitter);
            EXPECT_NE(upscaled.mJitter, wavelet.mJitter) << "and the same request, a different jitter";
            EXPECT_TRUE(upscaled.mFilterSuppressed) << "the wavelet was wanted and did not run";
            EXPECT_TRUE(upscaled.mJitterForced) << "and the frame jittered though nothing asked";
            EXPECT_EQ(upscaled.mUpscale, Upscale::Quality);
            EXPECT_EQ(upscaled.mPreset, Preset::E) << "the network a run pins is the one it reports";

            // Asking for exactly what an upscaler does anyway is not an override, and saying it was
            // would put a note on every frame that read the manual first.
            const Reconstruction agreed = Reconstruction::resolve(
                Upscale::Performance, ReconstructionRequest{ .mFilter = false, .mJitter = true });
            EXPECT_EQ(agreed.mDenoiser, Denoiser::RayReconstruction);
            EXPECT_FALSE(agreed.mFilterSuppressed) << "no wavelet was wanted, so none was suppressed";
            EXPECT_FALSE(agreed.mJitterForced) << "jitter was asked for outright";

            // A preset is a statement about a network, so where none runs there is none to report.
            EXPECT_EQ(wavelet.mPreset, Preset::Default) << "no network ran, so no preset did";
            EXPECT_EQ(wavelet.mUpscale, Upscale::Off);
        }

        /// Every name round-trips, because a report is only worth anything if it reads back.
        TEST(RtxReconstructionTest, everyPresetAndDenoiserHasANameThatReadsBack)
        {
            for (const Preset preset : { Preset::Default, Preset::D, Preset::E })
                EXPECT_EQ(presetNamed(presetName(preset)), preset) << "round trip through " << presetName(preset);

            EXPECT_EQ(presetNamed("D"), std::nullopt) << "spelled as the SDK's letter and not as a capital";
            EXPECT_EQ(presetNamed("transformer"), std::nullopt) << "refused rather than defaulted";

            // Distinct, so a report cannot say two things with one word.
            EXPECT_NE(denoiserName(Denoiser::None), denoiserName(Denoiser::Wavelet));
            EXPECT_NE(denoiserName(Denoiser::Wavelet), denoiserName(Denoiser::RayReconstruction));
        }
    }
}
