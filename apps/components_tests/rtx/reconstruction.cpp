#include <optional>

#include <gtest/gtest.h>

#include <components/rtx/reconstruction.hpp>
#include <components/rtx/renderer.hpp>

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
                = Reconstruction::resolve(Upscaling{}, ReconstructionRequest{ .mFilter = true, .mJitter = false });
            EXPECT_EQ(wavelet.mDenoiser, Denoiser::Wavelet);
            EXPECT_FALSE(wavelet.mJitter);
            EXPECT_FALSE(wavelet.mFilterSuppressed) << "nothing overruled it";
            EXPECT_FALSE(wavelet.mJitterForced);

            const Reconstruction raw
                = Reconstruction::resolve(Upscaling{}, ReconstructionRequest{ .mFilter = false, .mJitter = true });
            EXPECT_EQ(raw.mDenoiser, Denoiser::None) << "which is what a converged reference is built from";
            EXPECT_TRUE(raw.mJitter) << "and jitter is what makes that reference antialiased";

            // **The same request, and an upscaler in the way of it.** Both switches stop deciding:
            // the wavelet does not run because Ray Reconstruction is itself the denoiser, and the
            // frame jitters because reconstruction across frames of one sample point is
            // reconstruction from one sample. Neither of those is new behaviour; what is new is that
            // the answer says both happened.
            const Reconstruction upscaled
                = Reconstruction::resolve(Upscaling{ .mMode = Upscale::Quality, .mPreset = Preset::E },
                    ReconstructionRequest{ .mFilter = true, .mJitter = false });
            EXPECT_EQ(upscaled.mDenoiser, Denoiser::RayReconstruction);
            EXPECT_NE(upscaled.mDenoiser, wavelet.mDenoiser) << "the same request, a different denoiser";
            EXPECT_TRUE(upscaled.mJitter);
            EXPECT_NE(upscaled.mJitter, wavelet.mJitter) << "and the same request, a different jitter";
            EXPECT_TRUE(upscaled.mFilterSuppressed) << "the wavelet was wanted and did not run";
            EXPECT_TRUE(upscaled.mJitterForced) << "and the frame jittered though nothing asked";
            EXPECT_EQ(upscaled.mUpscaling.mMode, Upscale::Quality);
            EXPECT_EQ(upscaled.mUpscaling.mPreset, Preset::E) << "the network a run pins is the one it reports";

            // Asking for exactly what an upscaler does anyway is not an override, and saying it was
            // would put a note on every frame that read the manual first.
            const Reconstruction agreed = Reconstruction::resolve(
                Upscaling{ .mMode = Upscale::Performance }, ReconstructionRequest{ .mFilter = false, .mJitter = true });
            EXPECT_EQ(agreed.mDenoiser, Denoiser::RayReconstruction);
            EXPECT_FALSE(agreed.mFilterSuppressed) << "no wavelet was wanted, so none was suppressed";
            EXPECT_FALSE(agreed.mJitterForced) << "jitter was asked for outright";

            // A preset is a statement about a network, so where none runs there is none to report.
            EXPECT_EQ(wavelet.mUpscaling.mPreset, Preset::Default) << "no network ran, so no preset did";
            EXPECT_EQ(wavelet.mUpscaling.mMode, Upscale::Off);
        }

        /// The accumulated bounce exists only where the wavelet ran.
        ///
        /// **The rule `readChannel` asserts on**, so a caller that wants the firefly tail asks
        /// first and is told which denoiser ran, rather than aborting inside the backend.
        TEST(RtxReconstructionTest, onlyAWaveletFrameCarriesTheAccumulatedBounce)
        {
            const Reconstruction wavelet
                = Reconstruction::resolve(Upscaling{}, ReconstructionRequest{ .mFilter = true });
            const Reconstruction raw = Reconstruction::resolve(Upscaling{}, ReconstructionRequest{ .mFilter = false });
            const Reconstruction upscaled = Reconstruction::resolve(
                Upscaling{ .mMode = Upscale::Quality }, ReconstructionRequest{ .mFilter = true });

            EXPECT_TRUE(wavelet.filtered());
            EXPECT_FALSE(raw.filtered()) << "nothing denoised it, which is what a reference is built from";
            EXPECT_FALSE(upscaled.filtered()) << "Ray Reconstruction is the denoiser, and it is not this one";

            EXPECT_TRUE(hasFrameImage(wavelet, FrameImage::Accumulated));
            EXPECT_FALSE(hasFrameImage(raw, FrameImage::Accumulated));
            EXPECT_FALSE(hasFrameImage(upscaled, FrameImage::Accumulated)) << "the same ask, and no channel to read";

            // The composite's own output is there whatever put the frame back together, and every
            // g-buffer channel is the trace's, so `hasFrameImage` has one question to answer.
            for (const Reconstruction& put : { wavelet, raw, upscaled })
                EXPECT_TRUE(hasFrameImage(put, FrameImage::Composite));
        }

        /// Every name round-trips, because a report is only worth anything if it reads back.
        TEST(RtxReconstructionTest, everyPresetAndDenoiserHasANameThatReadsBack)
        {
            for (const Preset preset : { Preset::Default, Preset::D, Preset::E })
                EXPECT_EQ(sPresetNames.named(sPresetNames.name(preset)), preset)
                    << "round trip through " << sPresetNames.name(preset);

            EXPECT_EQ(sPresetNames.named("D"), std::nullopt) << "spelled as the SDK's letter and not as a capital";
            EXPECT_EQ(sPresetNames.named("transformer"), std::nullopt) << "refused rather than defaulted";

            // Distinct, so a report cannot say two things with one word.
            EXPECT_NE(sDenoiserNames.name(Denoiser::None), sDenoiserNames.name(Denoiser::Wavelet));
            EXPECT_NE(sDenoiserNames.name(Denoiser::Wavelet), sDenoiserNames.name(Denoiser::RayReconstruction));
        }
    }
}
