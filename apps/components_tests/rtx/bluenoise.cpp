#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <numbers>
#include <numeric>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtx/bluenoise.hpp>

namespace Rtx
{
    namespace
    {
        constexpr std::size_t sExtent = Shaders::BLUE_NOISE_EXTENT;
        constexpr std::size_t sCount = sExtent * sExtent;

        /// One channel of the tile, as a plain field.
        std::vector<float> channel(std::uint32_t which)
        {
            const std::span<const float> values = BlueNoise::shared().getValues();

            std::vector<float> field(sCount);
            for (std::size_t i = 0; i < sCount; ++i)
                field[i] = values[i * Shaders::RANDOM_STREAMS + which];

            return field;
        }

        /// How much of the field's power sits at each distance from the origin in frequency.
        ///
        /// The mean is taken out first, so the enormous zero-frequency term — which says only how
        /// bright the field is on average and nothing about its arrangement — does not drown
        /// everything else. Distances wrap, because the tile does.
        ///
        /// A plain O(n^2) transform over four thousand cells, which is sixteen million operations
        /// and runs in well under a second. A fast one would be a second implementation to be right
        /// about for no gain here.
        std::vector<double> radialPower(const std::vector<float>& field)
        {
            const double mean = std::accumulate(field.begin(), field.end(), 0.0) / static_cast<double>(sCount);

            std::vector<double> power(sExtent / 2 + 1, 0.0);
            std::vector<double> counts(sExtent / 2 + 1, 0.0);

            for (std::size_t v = 0; v < sExtent; ++v)
                for (std::size_t u = 0; u < sExtent; ++u)
                {
                    std::complex<double> sum;
                    for (std::size_t y = 0; y < sExtent; ++y)
                        for (std::size_t x = 0; x < sExtent; ++x)
                        {
                            const double turns = static_cast<double>(u * x + v * y) / static_cast<double>(sExtent);
                            sum += (static_cast<double>(field[y * sExtent + x]) - mean)
                                * std::polar(1.0, -2.0 * std::numbers::pi_v<double> * turns);
                        }

                    const std::size_t du = std::min(u, sExtent - u);
                    const std::size_t dv = std::min(v, sExtent - v);
                    const auto radius = static_cast<std::size_t>(
                        std::lround(std::hypot(static_cast<double>(du), static_cast<double>(dv))));

                    if (radius < power.size())
                    {
                        power[radius] += std::norm(sum);
                        counts[radius] += 1.0;
                    }
                }

            for (std::size_t r = 0; r < power.size(); ++r)
                if (counts[r] > 0.0)
                    power[r] /= counts[r];

            return power;
        }

        /// The share of a field's power below `sExtent / 8` — the blotches a filter cannot remove.
        ///
        /// Everything but the zero-frequency term, which the transform has already taken out.
        double lowFrequencyShare(const std::vector<float>& field)
        {
            const std::vector<double> power = radialPower(field);
            const double total = std::accumulate(power.begin() + 1, power.end(), 0.0);
            const double low = std::accumulate(power.begin() + 1, power.begin() + sExtent / 8, 0.0);

            return low / total;
        }

        /// The same values in a random order, which is what the tile is an improvement on.
        std::vector<float> shuffledSameValues()
        {
            std::vector<std::uint32_t> ranks(sCount);
            std::iota(ranks.begin(), ranks.end(), std::uint32_t{ 0 });
            std::shuffle(ranks.begin(), ranks.end(), std::mt19937(12345));

            std::vector<float> field(sCount);
            for (std::size_t i = 0; i < sCount; ++i)
                field[i] = (static_cast<float>(ranks[i]) + 0.5f) / static_cast<float>(sCount);

            return field;
        }

        /// Every channel holds each rank exactly once, which is what makes the values uniform.
        ///
        /// **Not a statistical claim but an exact one.** Void-and-cluster ranks the pixels, so the
        /// values are the stratified sequence `(i + 0.5) / count` dealt out over the tile — every
        /// draw from it is uniform by construction rather than on average, and a sampler offset by
        /// it inherits that. A generator that lost or repeated a rank would still look like noise.
        TEST(RtxBlueNoiseTest, everyChannelIsAPermutationOfTheSameStratifiedValues)
        {
            const std::span<const float> values = BlueNoise::shared().getValues();
            ASSERT_EQ(values.size(), sCount * Shaders::RANDOM_STREAMS);

            for (std::uint32_t which = 0; which < Shaders::RANDOM_STREAMS; ++which)
            {
                std::vector<std::uint32_t> ranks;
                ranks.reserve(sCount);
                for (const float value : channel(which))
                {
                    EXPECT_GT(value, 0.0f) << "channel " << which;
                    EXPECT_LT(value, 1.0f) << "channel " << which;
                    ranks.push_back(static_cast<std::uint32_t>(std::lround(value * sCount - 0.5f)));
                }

                std::sort(ranks.begin(), ranks.end());

                std::vector<std::uint32_t> expected(sCount);
                std::iota(expected.begin(), expected.end(), std::uint32_t{ 0 });
                EXPECT_EQ(ranks, expected) << "channel " << which << " is a permutation of its ranks";
            }
        }

        /// The channels differ, which is the only thing that keeps two draws by one pixel apart.
        ///
        /// They were the same number once — the fog's march offset and the bounce's elevation — and
        /// a tile whose channels agreed would put that back without changing a line of the shader.
        TEST(RtxBlueNoiseTest, theChannelsAreNotCopiesOfEachOther)
        {
            for (std::uint32_t which = 1; which < Shaders::RANDOM_STREAMS; ++which)
            {
                const std::vector<float> first = channel(0);
                const std::vector<float> other = channel(which);

                std::size_t same = 0;
                for (std::size_t i = 0; i < sCount; ++i)
                    same += first[i] == other[i] ? 1u : 0u;

                // Two independent permutations of four thousand values agree in one place on
                // average, so anything past a handful is not chance.
                EXPECT_LT(same, std::size_t{ 16 }) << "channel " << which << " against channel 0";
            }
        }

        /// The tile has next to no low-frequency power, and the same values shuffled have plenty.
        ///
        /// **This is the whole claim, and it is only a claim about arrangement.** Both fields below
        /// hold the identical four thousand values and are identically uniform; a test of their
        /// histograms could not tell them apart. What differs is where the power sits: void-and-
        /// cluster spends it all above the eighth of the spectrum measured here, and a shuffle
        /// spreads it evenly, which puts a fifth of it underneath.
        ///
        /// It matters because that fifth is what survives a denoiser. Fine grain averages away
        /// against its neighbours; a low-frequency blotch is indistinguishable from lighting that is
        /// genuinely smooth, so a filter either keeps it or destroys the real thing beside it.
        ///
        /// The bar is a hundredth of the shuffle's share. Measured, the two are 0.01% against 20%,
        /// so this passes by more than two orders and would fail the moment the search stopped
        /// working — a mask left at its initial random arrangement lands on the shuffle's figure.
        TEST(RtxBlueNoiseTest, theTileSpendsItsPowerHighWhereTheSameValuesShuffledSpreadItEvenly)
        {
            const double shuffled = lowFrequencyShare(shuffledSameValues());
            const double tiled = lowFrequencyShare(channel(0));

            EXPECT_GT(shuffled, 0.1) << "a shuffle puts a good share of its power low";
            EXPECT_LT(tiled, shuffled * 0.01) << "and the tile puts a hundredth of that there";
        }
    }
}
