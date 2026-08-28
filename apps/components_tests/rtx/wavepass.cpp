#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtx/shaders/wave.h>
#include <components/rtx/wavecascade.hpp>
#include <components/rtx/wavespectrum.hpp>
#include <components/rtxvulkan/commands.hpp>
#include <components/rtxvulkan/device.hpp>
#include <components/rtxvulkan/wavepass.hpp>

#include "harness.hpp"
#include "wavemoments.hpp"

namespace Rtx
{
    namespace
    {
        /// Runs one synthesis at `seconds` and hands back the pass that holds it.
        void synthesise(const WavePass& waves, CommandPool& pool, float seconds)
        {
            pool.submitAndWait([&](VkCommandBuffer commands) { waves.record(commands, seconds); });
        }

        /// The last level of every chain is the sea state itself.
        ///
        /// **What the shader reads off it, and the whole reason the moments ride in a fourth
        /// channel.** A one-texel level is the mean of the tile, so `E[h^2]` off the surface is the
        /// surface's variance, `E[|s|^2]` off the curvature is its mean square slope, and the
        /// variance texture's own last level is how far the curvature's trace fluctuates. All three
        /// are sums over the amplitudes that a transform and nine halvings must not have moved.
        TEST(RtxWavePassTest, theLastLevelOfEachChainIsTheSeaStateItself)
        {
            std::string reason;
            Testing::Harness* harness = Testing::getHarness(reason);
            if (harness == nullptr)
                GTEST_SKIP() << reason;

            const Device& device = *harness->mDevice;
            CommandPool pool(device);
            const WavePass waves(device, pool, Testing::getShaderDirectory());

            synthesise(waves, pool, 0.0f);

            const SeaState sea;
            const std::array<WaveCascade, Shaders::WAVE_CASCADES> cascades = makeWaveCascades(sea);

            float elevation = 0.0f;
            float slope = 0.0f;
            float trace = 0.0f;

            for (std::size_t cascade = 0; cascade < Shaders::WAVE_CASCADES; ++cascade)
            {
                const Image& surface = waves.getSurface(cascade);
                const Image& curvature = waves.getCurvature(cascade);
                const Image& variance = waves.getVariance(cascade);

                const std::uint32_t last = surface.getMipLevels() - 1;
                ASSERT_EQ(surface.getWidthAt(last), 1u) << "cascade " << cascade << " does not reduce to one texel";

                const std::vector<float> mean = Testing::readHalves(pool, surface, last);
                const std::vector<float> curved = Testing::readHalves(pool, curvature, last);
                const std::vector<float> traced = Testing::readHalves(pool, variance, variance.getMipLevels() - 1);

                // A field of zero mean, which is what a spectrum with no entry at the origin means
                // and what every variance below is taken about. Against the surface's own root mean
                // square, so the tolerance is a share of the sea rather than a number of units.
                const float roughness = std::sqrt(Testing::momentOf(cascades[cascade], 0));
                EXPECT_NEAR(mean[0], 0.0f, 0.02f * roughness) << "the mean elevation of cascade " << cascade;

                elevation += mean[3];
                slope += curved[3];
                trace += traced[0];
            }

            float wantedElevation = 0.0f;
            float wantedSlope = 0.0f;
            float wantedTrace = 0.0f;
            for (const WaveCascade& cascade : cascades)
            {
                wantedElevation += Testing::momentOf(cascade, 0);
                wantedSlope += Testing::momentOf(cascade, 2);
                wantedTrace += Testing::momentOf(cascade, 4);
            }

            // **And the number the shader is handed rather than made to fetch.** `mWaveSlope` carries
            // this same sum to the caustic's band limit, so a `WavePass` whose slope disagreed with
            // its own chains would blur the pattern by the wrong amount at every depth.
            EXPECT_NEAR(waves.getSlope(), std::sqrt(wantedSlope), 1e-4f * std::sqrt(wantedSlope))
                << "the slope the pass hands over";

            // A per cent, which is what nine halvings of a half-float image cost: each level is the
            // mean of four texels rounded to eleven bits of mantissa.
            EXPECT_NEAR(elevation, wantedElevation, 0.01f * wantedElevation) << "the surface's variance";
            EXPECT_NEAR(slope, wantedSlope, 0.01f * wantedSlope) << "the mean square slope";
            EXPECT_NEAR(trace, wantedTrace, 0.02f * wantedTrace) << "the variance of the curvature's trace";
        }

        /// A level of a chain is the mean of the four texels above it.
        ///
        /// **The property the cone's arithmetic rests on.** `waveLevel` picks a level by the width
        /// of the footprint, and reads `E[f]` and `E[f^2]` off it as though the level were a box
        /// average of what it covers. A blit with a linear filter is exactly that, and this is what
        /// says so — a driver that took the nearest texel instead would leave the variance at zero
        /// and every distant sea polished.
        TEST(RtxWavePassTest, aLevelOfTheChainIsTheMeanOfWhatItCovers)
        {
            std::string reason;
            Testing::Harness* harness = Testing::getHarness(reason);
            if (harness == nullptr)
                GTEST_SKIP() << reason;

            const Device& device = *harness->mDevice;
            CommandPool pool(device);
            const WavePass waves(device, pool, Testing::getShaderDirectory());

            synthesise(waves, pool, 0.0f);

            // The narrow tile, because the assertion is about the filter and not about the size.
            constexpr std::size_t cascade = Shaders::WAVE_CASCADES - 1;
            const Image& surface = waves.getSurface(cascade);
            const std::uint32_t width = surface.getWidth();

            const std::vector<float> fine = Testing::readHalves(pool, surface, 0);
            const std::vector<float> coarse = Testing::readHalves(pool, surface, 1);

            ASSERT_EQ(fine.size(), std::size_t{ width } * width * 4);
            ASSERT_EQ(coarse.size(), std::size_t{ width } / 2 * (width / 2) * 4);

            // How large the numbers being averaged are, so the tolerance below is a share of the
            // field rather than a number of world units.
            float scale = 0.0f;
            for (std::size_t at = 0; at < fine.size(); at += 4)
                scale = std::max(scale, std::abs(fine[at]));

            for (std::uint32_t y = 0; y < width / 2; ++y)
                for (std::uint32_t x = 0; x < width / 2; ++x)
                {
                    const auto height = [&](std::uint32_t column, std::uint32_t row) {
                        return fine[(std::size_t{ row } * width + column) * 4];
                    };

                    const float box = 0.25f
                        * (height(2 * x, 2 * y) + height(2 * x + 1, 2 * y) + height(2 * x, 2 * y + 1)
                            + height(2 * x + 1, 2 * y + 1));

                    ASSERT_NEAR(coarse[(std::size_t{ y } * (width / 2) + x) * 4], box, 0.005f * scale)
                        << "at " << x << ", " << y;
                }

            // **And what that box took out of the slope is what the spectrum says it would.** A
            // level is a box filter, so it passes a wavevector at `sinc(kx w / 2) sinc(ky w / 2)`
            // and loses the rest — and the shader reads exactly that loss as `curve.w` less the
            // square of the slope it kept. `Testing::lostSlopeOf` is the same statement over the
            // amplitudes, so this is the chain against the spectrum with the filter written out.
            //
            // An integer level, because a fraction of one is a blend of two boxes and this is about
            // the box.
            const std::vector<float> slopes = Testing::readHalves(pool, surface, 1);
            const std::vector<float> squared = Testing::readHalves(pool, waves.getCurvature(cascade), 1);

            float lost = 0.0f;
            for (std::size_t at = 0; at < squared.size(); at += 4)
                lost += squared[at + 3] - slopes[at + 1] * slopes[at + 1] - slopes[at + 2] * slopes[at + 2];

            lost /= static_cast<float>(squared.size() / 4);

            // The default state, because that is the one `WavePass` describes itself with when it is
            // built and nothing here has described another.
            const std::array<WaveCascade, Shaders::WAVE_CASCADES> drawn = makeWaveCascades(SeaState{});
            const float texel = sWaveTiles[cascade].mExtent / static_cast<float>(sWaveTiles[cascade].mGrid);

            const float wanted = Testing::lostSlopeOf(drawn[cascade], 2.0f * texel);

            EXPECT_NEAR(lost, wanted, 0.025f * wanted) << "the slope one halving averaged away";
        }

        /// The sea moves, and it moves at the speed its own dispersion relation gives.
        ///
        /// **A field that only turned its phases would translate rather than beat.** Every entry
        /// turns at its own `omega`, so a second later the surface is a different surface and not the
        /// same one shifted — which is what a correlation well under one says and what a rigid
        /// translation could not.
        TEST(RtxWavePassTest, theSurfaceAtAnotherMomentIsAnotherSurface)
        {
            std::string reason;
            Testing::Harness* harness = Testing::getHarness(reason);
            if (harness == nullptr)
                GTEST_SKIP() << reason;

            const Device& device = *harness->mDevice;
            CommandPool pool(device);
            const WavePass waves(device, pool, Testing::getShaderDirectory());

            const Image& surface = waves.getSurface(0);

            synthesise(waves, pool, 0.0f);
            const std::vector<float> before = Testing::readHalves(pool, surface, 0);

            synthesise(waves, pool, 2.0f);
            const std::vector<float> after = Testing::readHalves(pool, surface, 0);

            ASSERT_EQ(before.size(), after.size());

            float together = 0.0f;
            float first = 0.0f;
            float second = 0.0f;
            for (std::size_t at = 0; at < before.size(); at += 4)
            {
                together += before[at] * after[at];
                first += before[at] * before[at];
                second += after[at] * after[at];
            }

            // The same sea, so the two hold the same energy however far it has run.
            EXPECT_NEAR(second, first, 0.02f * first) << "two seconds later, a different amount of sea";

            // And two seconds is long enough for the swell — 420 units at about a two-second period
            // — to have carried the field somewhere else entirely.
            const float correlation = together / std::sqrt(first * second);
            EXPECT_LT(std::abs(correlation), 0.5f) << "measured " << correlation;

            // A run of nought is the same field twice, which is what makes a screenshot repeatable.
            synthesise(waves, pool, 0.0f);
            const std::vector<float> again = Testing::readHalves(pool, surface, 0);
            EXPECT_EQ(again, before) << "the same moment is not the same sea";
        }
    }
}
