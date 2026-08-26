#include <algorithm>
#include <cmath>

#include <gtest/gtest.h>

#include <components/rtx/wavespectrum.hpp>

namespace Rtx
{
    namespace
    {
        constexpr std::size_t sBands = 8;
        constexpr std::size_t sPerBand = Shaders::WAVE_COUNT / sBands;

        /// How wide the directions of one band fan, in radians.
        float fanOf(const std::array<Shaders::GpuWave, Shaders::WAVE_COUNT>& waves, std::size_t band)
        {
            const osg::Vec2f first = waves[band * sPerBand].mDirection;
            const osg::Vec2f last = waves[band * sPerBand + sPerBand - 1].mDirection;

            return std::acos(std::clamp(first * last, -1.0f, 1.0f));
        }

        /// The table is scaled to the height that was asked for, whatever the spectrum's own was.
        ///
        /// JONSWAP's `alpha` is a fetch-and-wind parameter nothing here knows, and every term in it
        /// is a constant multiplier — so it cancels, and the one number a person can picture is put
        /// in its place. Significant height is four times the standard deviation of the surface, and
        /// a sinusoid of amplitude `A` contributes `A^2 / 2` of variance.
        TEST(RtxWaveSpectrumTest, theTableCarriesTheSignificantHeightItWasAskedFor)
        {
            for (const float height : { 4.0f, 9.4f, 40.0f })
            {
                SeaState sea;
                sea.mSignificantHeight = height;

                float variance = 0.0f;
                for (const Shaders::GpuWave& wave : sea.getWaves())
                    variance += wave.mAmplitude * wave.mAmplitude * 0.5f;

                EXPECT_NEAR(4.0f * std::sqrt(variance), height, height * 1e-4f) << "at height " << height;
            }
        }

        /// Every component moves at the speed the dispersion relation gives its own wavenumber.
        ///
        /// `omega^2 = g k tanh(k h)`, of which deep water's `sqrt(g k)` is only the limit — a wave
        /// whose length approaches the depth falls behind it, which is why a swell slows and steepens
        /// as it reaches a shore. A table whose speeds did not come from its wavenumbers would drift
        /// out of phase with the caustics that differentiate the same field.
        TEST(RtxWaveSpectrumTest, everyComponentTravelsAtItsOwnDispersionSpeed)
        {
            const SeaState sea;
            for (const Shaders::GpuWave& wave : sea.getWaves())
            {
                EXPECT_NEAR(wave.mSpeed, sea.getFrequency(wave.mWavenumber), wave.mSpeed * 1e-4f)
                    << "at wavenumber " << wave.mWavenumber;
                EXPECT_NEAR(wave.mDirection.length(), 1.0f, 1e-5f);
            }
        }

        /// The shelf is what makes this TMA rather than JONSWAP.
        ///
        /// A shelf cannot carry a wave whose orbit reaches the bottom, so shallow water both slows
        /// the long components and takes energy out of them. Both must show, or the depth term is
        /// decoration.
        TEST(RtxWaveSpectrumTest, aShallowerShelfSlowsTheLongWavesAndTakesTheirEnergy)
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

            // And its share of the energy moves up the table: with both scaled to the same height,
            // shallow water puts less of it into the longest band and more into the shortest.
            const auto deepWaves = deep.getWaves();
            const auto shallowWaves = shallow.getWaves();

            EXPECT_LT(shallowWaves[0].mAmplitude, deepWaves[0].mAmplitude) << "less swell";
            EXPECT_GT(shallowWaves[7 * sPerBand].mAmplitude, deepWaves[7 * sPerBand].mAmplitude) << "and more chop";
        }

        /// The spread is frequency-dependent, which is what stops a sum of plane waves being a grid.
        ///
        /// Donelan-Banner: the swell arrives as near-parallel trains and the chop fans wide. A
        /// spectrum spread evenly over directions draws a lattice, and curvature — which weights a
        /// component by `A k^2` — is where that shows first.
        TEST(RtxWaveSpectrumTest, theSwellArrivesNarrowAndTheChopFansWide)
        {
            const auto waves = SeaState{}.getWaves();

            const float swell = fanOf(waves, 0);
            const float chop = fanOf(waves, sBands - 1);

            EXPECT_GT(chop, swell * 2.0f) << "swell " << swell << " against chop " << chop;

            // And the swell's fan is what Donelan-Banner says it is, not merely narrower than the
            // chop's — a ratio holds however far the quantiles are compressed toward the middle.
            // The lowest band sits at 0.7 of the peak, so its spread is
            //
            //   2.61 * 0.7^1.3 = 1.6418
            //
            // and its outermost directions lie at the 0.125 and 0.875 quantiles of a `sech^2` of
            // that width, which is `atanh(0.75 * tanh(1.6418 pi)) / 1.6418 = 0.5925` either side:
            // a fan of 1.185 radians, or the 68 degrees the spectrum's own description quotes.
            EXPECT_NEAR(swell, 1.185f, 0.005f);

            // Every band takes its own heading off the last by the golden angle, so no two share a
            // direction however far their fans reach.
            for (std::size_t band = 1; band < sBands; ++band)
            {
                const osg::Vec2f previous = waves[(band - 1) * sPerBand].mDirection;
                const osg::Vec2f current = waves[band * sPerBand].mDirection;
                EXPECT_GT(std::acos(std::clamp(previous * current, -1.0f, 1.0f)), 0.1f) << "band " << band;
            }
        }

        /// Every component of a band carries the same energy, which is what quantile sampling buys.
        ///
        /// The share of a band's energy between two quantiles of its spread is equal by
        /// construction, so the spread's shape is exact however few directions are taken — where
        /// sampling at even angles would need many to describe a narrow fan.
        ///
        /// Their wavelengths still differ: a band stands for a *range*, and giving all its
        /// directions the middle of it gives them one speed too, so they would translate as a rigid
        /// pattern rather than beat against one another.
        TEST(RtxWaveSpectrumTest, aBandsDirectionsShareItsEnergyAndNotItsWavelength)
        {
            const auto waves = SeaState{}.getWaves();

            for (std::size_t band = 0; band < sBands; ++band)
            {
                const Shaders::GpuWave& first = waves[band * sPerBand];
                for (std::size_t within = 1; within < sPerBand; ++within)
                {
                    const Shaders::GpuWave& other = waves[band * sPerBand + within];
                    EXPECT_NEAR(other.mAmplitude, first.mAmplitude, first.mAmplitude * 1e-5f)
                        << "band " << band << " direction " << within;
                    EXPECT_NE(other.mWavenumber, first.mWavenumber) << "band " << band;
                }
            }
        }

        /// The table spans from the swell down to the shortest wave worth carrying.
        ///
        /// The short end is a limit in time as much as in space: curvature climbs with wavenumber so
        /// the shortest waves decide the caustics, and a wave's period falls with its length so they
        /// also decide how fast the pattern reshuffles. Thirty-two units is where those two meet.
        TEST(RtxWaveSpectrumTest, theBandsRunFromTheSwellDownToThirtyOddUnits)
        {
            float longest = 0.0f;
            float shortest = 1.0e9f;
            for (const Shaders::GpuWave& wave : SeaState{}.getWaves())
            {
                const float wavelength = Shaders::TAU / wave.mWavenumber;
                longest = std::max(longest, wavelength);
                shortest = std::min(shortest, wavelength);
            }

            // The bands are *centred* on their spacing and each spans half a band either side, so
            // the table reaches past the 420-unit peak at one end and past 32 units at the other.
            EXPECT_GT(longest, 700.0f);
            EXPECT_LT(longest, 1200.0f);
            EXPECT_GT(shortest, 20.0f);
            EXPECT_LT(shortest, 32.0f);
        }
    }
}
