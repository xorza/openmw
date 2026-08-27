#include <cmath>
#include <cstddef>

#include <gtest/gtest.h>

#include <components/rtx/wavecascade.hpp>
#include <components/rtx/wavespectrum.hpp>

namespace Rtx
{
    namespace
    {
        /// The wavelength the entry at `at` stands for, or zero for the wavevector of no length at
        /// the middle.
        float wavelengthAt(const WaveCascade& cascade, std::size_t at)
        {
            const int half = static_cast<int>(cascade.mGrid) / 2;
            const int row = static_cast<int>(at / cascade.mGrid) - half;
            const int column = static_cast<int>(at % cascade.mGrid) - half;

            const float step = Shaders::TAU / cascade.mExtent;
            const float wavenumber
                = step * std::sqrt(static_cast<float>(row * row) + static_cast<float>(column * column));

            return wavenumber > 0.0f ? Shaders::TAU / wavenumber : 0.0f;
        }

        /// The variance of the surface these amplitudes describe.
        ///
        /// **Twice their sum, and the factor is the physics rather than a convention.** Each
        /// wavevector's contribution is `h0(k) e^{iwt} + conj(h0(-k)) e^{-iwt}` and the two draws
        /// are independent, so the mean square of the sum is the sum of the two — and Parseval
        /// carries that to the field. Written out here rather than taken off the builder, so a
        /// factor dropped there fails this rather than passing it.
        float varianceOf(const std::array<WaveCascade, Shaders::WAVE_CASCADES>& cascades)
        {
            float total = 0.0f;
            for (const WaveCascade& cascade : cascades)
                for (const osg::Vec2f& amplitude : cascade.mAmplitudes)
                    total += 2.0f * amplitude.length2();

            return total;
        }

        /// The tiles carry the significant height they were asked for, across all three.
        ///
        /// **Scaled together and not one at a time.** Each tile is an independent draw of a share of
        /// one spectrum, so their variances add — normalising each to the height asked for would
        /// give two seas of that roughness rather than one.
        TEST(RtxWaveCascadeTest, theTilesCarryTheSignificantHeightTheyWereAskedFor)
        {
            for (const float height : { 4.0f, 9.4f, 40.0f })
            {
                SeaState sea;
                sea.mSignificantHeight = height;

                EXPECT_NEAR(4.0f * std::sqrt(varianceOf(makeWaveCascades(sea))), height, height * 1e-4f)
                    << "at height " << height;
            }
        }

        /// Every tile holds the whole spectrum, and holds it in tens of thousands of components.
        ///
        /// **The component count is what the transform is for.** The sinusoid table this replaces
        /// carried sixty-four, of which the shortest four owned forty per cent of the curvature —
        /// four plane waves crossing, which is a lattice. A tile of this size holds five orders more
        /// than that inside the same band.
        ///
        /// **And the band is the spectrum's, not the tile's.** A tile cannot hold a wave longer than
        /// itself or shorter than two of its texels, and both of these are wide enough and fine
        /// enough that neither limit bites: what bounds the band is `sShortestWave`, which the
        /// spectrum stops at for reasons of its own.
        TEST(RtxWaveCascadeTest, everyTileHoldsTheWholeSpectrumInTensOfThousandsOfComponents)
        {
            const auto cascades = makeWaveCascades(SeaState{});

            for (std::size_t index = 0; index < Shaders::WAVE_CASCADES; ++index)
            {
                const WaveCascade& cascade = cascades[index];
                EXPECT_EQ(cascade.mExtent, sWaveTiles[index].mExtent);
                EXPECT_EQ(cascade.mGrid, sWaveTiles[index].mGrid);
                ASSERT_EQ(cascade.mAmplitudes.size(), cascade.mGrid * cascade.mGrid);

                const float nyquist = 2.0f * cascade.mExtent / static_cast<float>(cascade.mGrid);
                EXPECT_LT(nyquist, sShortestWave) << "tile " << index << " cannot reach the spectrum's short end";
                EXPECT_GT(cascade.mExtent, 1000.0f) << "tile " << index << " cannot hold the swell";

                std::size_t carried = 0;
                for (std::size_t at = 0; at < cascade.mAmplitudes.size(); ++at)
                {
                    if (cascade.mAmplitudes[at] == osg::Vec2f())
                        continue;

                    const float wavelength = wavelengthAt(cascade, at);
                    ASSERT_LE(wavelength, cascade.mExtent) << "tile " << index << " holds a wave it cannot fit";
                    ASSERT_GT(wavelength, sShortestWave) << "tile " << index << " holds a wave past the cutoff";

                    ++carried;
                }

                // The band runs from the tile's own width down to `sShortestWave`, so it fills the
                // disc of radius `mExtent / sShortestWave` — 51 420 cells of the wide tile and 7088
                // of the narrow one. Half of that is a floor no rounding reaches.
                const float radius = cascade.mExtent / sShortestWave;
                EXPECT_GT(static_cast<float>(carried), 1.5f * radius * radius)
                    << "tile " << index << " carries too few components to be water";
            }
        }

        /// No two tiles share a period, which is the whole reason there are two.
        ///
        /// One tile lays the same water down every `mExtent` units. Two whose widths divide into one
        /// another line up every few tiles and draw one coarse grid over the sea — the artefact the
        /// sixty-four-sinusoid table drew on a seabed, moved up a scale. These three come back into
        /// step only after tens of thousands of units, which is past anything a frame contains.
        TEST(RtxWaveCascadeTest, noTwoTilesShareAPeriod)
        {
            for (std::size_t index = 1; index < Shaders::WAVE_CASCADES; ++index)
            {
                const float ratio = sWaveTiles[index - 1].mExtent / sWaveTiles[index].mExtent;

                // Within a twentieth of a whole number is close enough to line up over the handful
                // of tiles a frame covers, which is what this refuses.
                EXPECT_GT(std::abs(ratio - std::round(ratio)), 0.05f)
                    << "tiles " << index - 1 << " and " << index << " at " << sWaveTiles[index - 1].mExtent << " and "
                    << sWaveTiles[index].mExtent;
            }
        }

        /// Every entry turns at the speed the dispersion relation gives its own wavenumber.
        ///
        /// The same relation the sinusoid table uses, so a shallow shelf slows the swell here as it
        /// does there — and a tile whose entries turned at one speed would translate rigidly rather
        /// than beat into a sea.
        TEST(RtxWaveCascadeTest, everyEntryTurnsAtItsOwnDispersionSpeed)
        {
            const SeaState sea;
            const auto cascades = makeWaveCascades(sea);

            for (const WaveCascade& cascade : cascades)
                for (std::size_t at = 0; at < cascade.mAmplitudes.size(); ++at)
                {
                    if (cascade.mAmplitudes[at] == osg::Vec2f())
                        continue;

                    const float wavenumber = Shaders::TAU / wavelengthAt(cascade, at);
                    ASSERT_NEAR(cascade.mFrequencies[at], sea.getFrequency(wavenumber), 1e-4f)
                        << "at " << at << " of a tile " << cascade.mExtent << " across";
                }
        }

        /// The same sea state is the same sea, twice.
        ///
        /// **Every amplitude is a Gaussian draw**, and a draw with a history would give a cell a
        /// different surface every time it was loaded — and a screenshot a different one every time
        /// it was taken. The draws come off a hash of where they sit instead.
        TEST(RtxWaveCascadeTest, theSameSeaStateIsTheSameSeaTwice)
        {
            const SeaState sea;
            const auto first = makeWaveCascades(sea);
            const auto second = makeWaveCascades(sea);

            for (std::size_t index = 0; index < Shaders::WAVE_CASCADES; ++index)
                for (std::size_t at = 0; at < first[index].mAmplitudes.size(); ++at)
                    ASSERT_EQ(first[index].mAmplitudes[at], second[index].mAmplitudes[at])
                        << "tile " << index << ", entry " << at;

            // And another sea is another sea: the swell turns with the wind rather than the tiles
            // being redrawn around it, so the two differ everywhere off the new bearing.
            SeaState turned = sea;
            turned.mBearing = sea.mBearing + 1.0f;

            const auto third = makeWaveCascades(turned);

            std::size_t moved = 0;
            for (std::size_t at = 0; at < first[0].mAmplitudes.size(); ++at)
                moved += third[0].mAmplitudes[at] != first[0].mAmplitudes[at] ? 1 : 0;

            EXPECT_GT(moved, first[0].mAmplitudes.size() / 20) << "a different wind is a different sea";
        }
    }
}
