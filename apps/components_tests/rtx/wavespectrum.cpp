#include <gtest/gtest.h>

#include <components/misc/constants.hpp>
#include <components/rtx/wavespectrum.hpp>

namespace Rtx
{
    namespace
    {
        /// The two figures a shading language cannot include its way to.
        ///
        /// **`scene.h` is read by GLSL, so it can include no C++ header** — the game's own units
        /// and gravity are spelled again there, and a copy nothing checks is exactly the failure
        /// that file's own header comment exists to warn about. A `static_assert` cannot reach
        /// them: `const` is `const` and a `const float` is not a constant expression.
        TEST(RtxWaveSpectrumTest, theSharedConstantsAreTheGamesOwn)
        {
            EXPECT_EQ(Shaders::UNITS_PER_METRE, Constants::UnitsPerMeter);
            EXPECT_EQ(Shaders::WATER_GRAVITY, Constants::GravityConst * Constants::UnitsPerMeter);
        }

        /// The shelf is what makes this TMA rather than JONSWAP.
        ///
        /// A shelf cannot carry a wave whose orbit reaches the bottom, so shallow water slows the
        /// long components. Where that energy goes is `RtxWaveCascadeTest`'s question; this one is
        /// about the relation the whole spectrum is laid out through.
        TEST(RtxWaveSpectrumTest, aShallowerShelfSlowsTheLongWaves)
        {
            SeaState deep;
            deep.mDepth = 4000.0f;
            SeaState shallow;
            shallow.mDepth = 60.0f;

            // The same wavenumber travels slower over a shallower shelf, because `tanh(k h)` falls.
            // A 1257-unit wave over 60 units of water:
            //
            //   shallow = sqrt(627.1 * 0.005 * tanh(0.3)) = sqrt(0.9124) = 0.955
            //   deep    = sqrt(627.1 * 0.005 * tanh(20))  = sqrt(3.1355) = 1.771
            //
            // Fifty-four per cent of its open-sea speed, which is the coastal correction this
            // spectrum exists for. A shorter wave barely notices — at a wavenumber of 0.02 the same
            // shelf costs under a tenth.
            constexpr float wavenumber = 0.005f;
            EXPECT_NEAR(shallow.getFrequency(wavenumber), 0.955f, 0.002f);
            EXPECT_NEAR(deep.getFrequency(wavenumber), 1.771f, 0.002f);

            // And the inverse is the same relation walked back, which is what turns a wavelength on
            // a grid into the speed its entry turns at.
            EXPECT_NEAR(shallow.getWavenumber(shallow.getFrequency(wavenumber)), wavenumber, wavenumber * 1e-4f);
            EXPECT_NEAR(deep.getWavenumber(deep.getFrequency(wavenumber)), wavenumber, wavenumber * 1e-4f);
        }

        /// The density is highest at the wavelength it was asked to peak at.
        ///
        /// **What `mPeakWavelength` means, checked rather than assumed.** JONSWAP's sharpening term
        /// is centred on the peak and its tail falls as the fourth power of the ratio, so a spectrum
        /// whose maximum sat anywhere else would be one whose one legible dial did nothing.
        TEST(RtxWaveSpectrumTest, theDensityPeaksAtTheWavelengthItWasAskedFor)
        {
            const SeaState sea;
            const float peak = sea.getPeak();

            EXPECT_GT(sea.getEnergy(peak), sea.getEnergy(0.7f * peak));
            EXPECT_GT(sea.getEnergy(peak), sea.getEnergy(1.4f * peak));

            // And the tail falls away hard: `exp(-1.25 (peak / f)^4)` alone takes two thirds of it
            // by 0.7 of the peak, where the far side loses to the fifth power in the denominator.
            EXPECT_LT(sea.getEnergy(0.7f * peak), 0.4f * sea.getEnergy(peak));
            EXPECT_LT(sea.getEnergy(3.0f * peak), 0.02f * sea.getEnergy(peak));
        }

        /// The spread is frequency-dependent, which is what stops a spectrum drawing a grid.
        ///
        /// Donelan-Banner: the swell arrives as near-parallel trains and the chop fans wide. **Large
        /// is narrow** — the number is the width of a `sech^2`, so it falls as the fan opens.
        TEST(RtxWaveSpectrumTest, theSwellArrivesNarrowAndTheChopFansWide)
        {
            const SeaState sea;
            const float peak = sea.getPeak();

            // The three branches the fit is written in, each evaluated by hand:
            //
            //   0.7 of the peak: 2.61 * 0.7^1.3                          = 1.642
            //   the peak:        2.28 * 1^-1.3                           = 2.280
            //   3 of the peak:   10^(-0.4 + 0.8393 exp(-0.567 ln 9))     = 0.694
            EXPECT_NEAR(sea.getSpread(0.7f * peak), 1.642f, 0.002f);
            EXPECT_NEAR(sea.getSpread(peak), 2.280f, 0.002f);
            EXPECT_NEAR(sea.getSpread(3.0f * peak), 0.694f, 0.002f);

            // Which is the shape the whole thing is for: the chop's fan is three times the swell's.
            EXPECT_LT(sea.getSpread(3.0f * peak), 0.5f * sea.getSpread(0.7f * peak));
        }
    }
}
