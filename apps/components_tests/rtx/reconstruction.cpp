#include <optional>

#include <gtest/gtest.h>

#include <components/rtx/frameextents.hpp>
#include <components/rtx/reconstruction.hpp>
#include <components/rtx/upscale.hpp>

namespace Rtx
{
    namespace
    {
        /// What a frame with nothing upscaling is traced at, which is what it is shown at: the
        /// rule reads the extents under an upscaler alone, and the tests that raise one name
        /// their own.
        constexpr FrameExtents sUnscaled{ .mRenderWidth = 3840, .mOutputWidth = 3840 };

        /// DLSS's performance mode, which traces at half the width: exactly minus one level.
        constexpr FrameExtents sHalved{ .mRenderWidth = 1920, .mOutputWidth = 3840 };

        /// What an upscaler and a request decide between them: who denoises, and whether the
        /// sample point moves.
        ///
        /// **Worth a test of its own because it is a function of its inputs and nothing else** — no
        /// device, no scene, no frame.
        TEST(RtxReconstructionTest, anUpscalerDenoisesForItselfAndJittersWhateverWasAsked)
        {
            // Nothing upscaling: the two switches mean exactly what they say.
            const Reconstruction wavelet = Reconstruction::resolve(
                Upscaling{}, ReconstructionRequest{ .mFilter = true, .mJitter = false }, sUnscaled);
            EXPECT_EQ(wavelet.mDenoiser, Denoiser::Wavelet);
            EXPECT_FALSE(wavelet.mJitter);
            EXPECT_FALSE(wavelet.mFilterSuppressed) << "nothing overruled it";
            EXPECT_FALSE(wavelet.mJitterForced);

            const Reconstruction raw = Reconstruction::resolve(
                Upscaling{}, ReconstructionRequest{ .mFilter = false, .mJitter = true }, sUnscaled);
            EXPECT_EQ(raw.mDenoiser, Denoiser::None) << "which is what a converged reference is built from";
            EXPECT_TRUE(raw.mJitter) << "and jitter is what makes that reference antialiased";

            // **The same request, and an upscaler in the way of it.** Both switches stop deciding:
            // the wavelet does not run because Ray Reconstruction is itself the denoiser, and the
            // frame jitters because reconstruction across frames of one sample point is
            // reconstruction from one sample. Neither of those is new behaviour; what is new is that
            // the answer says both happened.
            const Reconstruction upscaled
                = Reconstruction::resolve(Upscaling{ .mMode = Upscale::Quality, .mPreset = Preset::E },
                    ReconstructionRequest{ .mFilter = true, .mJitter = false }, sHalved);
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
            const Reconstruction agreed = Reconstruction::resolve(Upscaling{ .mMode = Upscale::Performance },
                ReconstructionRequest{ .mFilter = false, .mJitter = true }, sHalved);
            EXPECT_EQ(agreed.mDenoiser, Denoiser::RayReconstruction);
            EXPECT_FALSE(agreed.mFilterSuppressed) << "no wavelet was wanted, so none was suppressed";
            EXPECT_FALSE(agreed.mJitterForced) << "jitter was asked for outright";

            // A preset is a statement about a network, so where none runs there is none to report.
            EXPECT_EQ(wavelet.mUpscaling.mPreset, Preset::Default) << "no network ran, so no preset did";
            EXPECT_EQ(wavelet.mUpscaling.mMode, Upscale::Off);
        }

        /// The two consequences the denoiser carries with it: where the trace draws from, and how
        /// far every texture level moves for the pixel that is shown rather than the one traced.
        TEST(RtxReconstructionTest, theDenoiserDecidesTheNoiseSourceAndTheUpscalerTheLevelBias)
        {
            // The wavelet reads the tile; it is the arrangement the filter is built for. With no
            // upscaler the ratio's term is nought — the traced pixel is the shown one — and the
            // epsilon is the whole of the bias, which is how a test reads a level off this path.
            const Reconstruction wavelet = Reconstruction::resolve(
                Upscaling{}, ReconstructionRequest{ .mFilter = true, .mLevelEpsilon = -0.5f }, sUnscaled);
            EXPECT_EQ(wavelet.mNoise, NoiseSource::BlueNoiseTile);
            EXPECT_FLOAT_EQ(wavelet.mLevelBias, -0.5f);
            EXPECT_EQ(Reconstruction::resolve(Upscaling{}, ReconstructionRequest{}, sUnscaled).mLevelBias, 0.0f)
                << "and nought where nothing was asked";

            // Ray Reconstruction asks for independent draws, so the hash. The bias is the guide's
            // log2(render / display): 1920 over 3840 is exactly minus one; balanced traces 2227 of
            // 3840 and reads log2(0.58) = -0.7860, which is the number a texture moves by.
            const Reconstruction performance
                = Reconstruction::resolve(Upscaling{ .mMode = Upscale::Performance }, ReconstructionRequest{}, sHalved);
            EXPECT_EQ(performance.mNoise, NoiseSource::WhiteHash);
            EXPECT_FLOAT_EQ(performance.mLevelBias, -1.0f);

            const Reconstruction balanced = Reconstruction::resolve(Upscaling{ .mMode = Upscale::Balanced },
                ReconstructionRequest{}, FrameExtents{ .mRenderWidth = 2227, .mOutputWidth = 3840 });
            EXPECT_NEAR(balanced.mLevelBias, -0.7860f, 0.0005f);

            // The epsilon is added past the ratio, and a request may name the source outright:
            // that is the A/B.
            const Reconstruction tuned = Reconstruction::resolve(Upscaling{ .mMode = Upscale::Performance },
                ReconstructionRequest{ .mNoise = NoiseSource::BlueNoiseTile, .mLevelEpsilon = -0.25f }, sHalved);
            EXPECT_EQ(tuned.mNoise, NoiseSource::BlueNoiseTile) << "asked for by name";
            EXPECT_FLOAT_EQ(tuned.mLevelBias, -1.25f);
        }

        /// The wavelet ran only where it was asked for and nothing upscaled: Ray Reconstruction is
        /// the denoiser under an upscaler, and it is not this one.
        TEST(RtxReconstructionTest, onlyAWaveletFrameIsFiltered)
        {
            const Reconstruction wavelet
                = Reconstruction::resolve(Upscaling{}, ReconstructionRequest{ .mFilter = true }, sUnscaled);
            const Reconstruction raw
                = Reconstruction::resolve(Upscaling{}, ReconstructionRequest{ .mFilter = false }, sUnscaled);
            const Reconstruction upscaled = Reconstruction::resolve(
                Upscaling{ .mMode = Upscale::Quality }, ReconstructionRequest{ .mFilter = true }, sHalved);

            EXPECT_TRUE(wavelet.filtered());
            EXPECT_FALSE(raw.filtered()) << "nothing denoised it, which is what a reference is built from";
            EXPECT_FALSE(upscaled.filtered()) << "Ray Reconstruction is the denoiser, and it is not this one";
        }

        /// The spellings a settings file and a report write: the SDK's preset letters in lower case,
        /// and `auto` left to the harness as its word for no override.
        TEST(RtxReconstructionTest, thePresetsAndNoiseSourcesAreSpelledAsASettingsFileWritesThem)
        {
            EXPECT_EQ(sPresetNames.name(Preset::Default), "default");
            EXPECT_EQ(sPresetNames.name(Preset::D), "d");
            EXPECT_EQ(sPresetNames.name(Preset::E), "e");
            EXPECT_EQ(sPresetNames.named("D"), std::nullopt) << "spelled as the SDK's letter and not as a capital";

            EXPECT_EQ(sNoiseSourceNames.name(NoiseSource::BlueNoiseTile), "blue-noise");
            EXPECT_EQ(sNoiseSourceNames.name(NoiseSource::WhiteHash), "white-hash");
            EXPECT_EQ(sNoiseSourceNames.named("auto"), std::nullopt)
                << "auto is the harness's word for no override, and not a source";
        }
    }
}
