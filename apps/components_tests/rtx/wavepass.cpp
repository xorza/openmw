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

        struct RtxWavePassTest : Testing::DeviceTest
        {
        };

        /// The last level of every chain is the sea state itself.
        ///
        /// **What the shader reads off it, and the whole reason the moments ride in a fourth
        /// channel.** A one-texel level is the mean of the tile, so `E[h^2]` off the surface is the
        /// surface's variance, `E[|s|^2]` off the curvature is its mean square slope, and the
        /// variance texture's own last level is how far the curvature's trace fluctuates. All three
        /// are sums over the amplitudes that a transform and nine halvings must not have moved.
        TEST_F(RtxWavePassTest, theLastLevelOfEachChainIsTheSeaStateItself)
        {
            const Device& device = getDevice();
            CommandPool pool(device);
            const WavePass waves(device, pool, Testing::getShaderDirectory());

            synthesise(waves, pool, 0.0f);

            const SeaState sea;
            const std::array<WaveCascade, Shaders::WAVE_CASCADES> cascades = makeWaveCascades(sea);

            float elevation = 0.0f;
            float slope = 0.0f;

            for (std::size_t cascade = 0; cascade < Shaders::WAVE_CASCADES; ++cascade)
            {
                const Image& surface = waves.getSurface(cascade);

                const std::uint32_t last = surface.getMipLevels() - 1;
                ASSERT_EQ(surface.getWidthAt(last), 1u) << "cascade " << cascade << " does not reduce to one texel";

                const std::vector<float> mean = Testing::readHalves(pool, surface, last);

                // A field of zero mean, which is what a spectrum with no entry at the origin means
                // and what every variance below is taken about. Against the field's own root mean
                // square, so the tolerance is a share of the sea rather than a number of units.
                const float tilt = std::sqrt(Testing::momentOf(cascades[cascade], 2));
                EXPECT_NEAR(mean[0], 0.0f, 0.02f * tilt) << "the mean slope of cascade " << cascade;

                slope += mean[2];
                elevation += mean[3];
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

            // **And the curvature the same way, which is what sizes the caustic's own fold.** There
            // is no chain of it left to read: the tile that carried `(tr H)^2` went when the fold
            // stopped being differenced per pixel, so this is the whole of what says the number the
            // shader divides by describes the water the shader is looking at.
            EXPECT_NEAR(waves.getMoments().mWhole, wantedTrace, 1e-4f * wantedTrace)
                << "the curvature the pass hands over";

            // A per cent, which is what nine halvings of a half-float image cost: each level is the
            // mean of four texels rounded to eleven bits of mantissa.
            EXPECT_NEAR(elevation, wantedElevation, 0.01f * wantedElevation) << "the surface's variance";
            EXPECT_NEAR(slope, wantedSlope, 0.01f * wantedSlope) << "the mean square slope";
        }

        /// A level of a chain is the mean of the four texels above it.
        ///
        /// **The property the cone's arithmetic rests on.** `waveLevel` picks a level by the width
        /// of the footprint, and reads `E[f]` and `E[f^2]` off it as though the level were a box
        /// average of what it covers. A blit with a linear filter is exactly that, and this is what
        /// says so — a driver that took the nearest texel instead would leave the variance at zero
        /// and every distant sea polished.
        TEST_F(RtxWavePassTest, aLevelOfTheChainIsTheMeanOfWhatItCovers)
        {
            const Device& device = getDevice();
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

            float lost = 0.0f;
            for (std::size_t at = 0; at < slopes.size(); at += 4)
                lost += slopes[at + 2] - slopes[at] * slopes[at] - slopes[at + 1] * slopes[at + 1];

            lost /= static_cast<float>(slopes.size() / 4);

            // The default state, because that is the one `WavePass` describes itself with when it is
            // built and nothing here has described another.
            const std::array<WaveCascade, Shaders::WAVE_CASCADES> drawn = makeWaveCascades(SeaState{});
            const float texel = sWaveTiles[cascade].mExtent / static_cast<float>(sWaveTiles[cascade].mGrid);

            const float wanted = Testing::lostSlopeOf(drawn[cascade], 2.0f * texel);

            EXPECT_NEAR(lost, wanted, 0.025f * wanted) << "the slope one halving averaged away";
        }

        /// The table of resolved curvature is what a sampler reading the chain would find.
        ///
        /// **The caustic's fold is read from a table now and not differenced out of a mip**, so the
        /// table has to describe what `textureLod` returns — which is not what the texels hold.
        /// A level's own transfer is Dirichlet's kernel, and then the tap reconstructs between its
        /// samples bilinearly, which is a second filter: it passes `(2 + cos(k w)) / 3` of a
        /// frequency's power over tap positions spread through a texel. Left out, the table stood at
        /// four thirds of the truth in the shallows and nearly three times it in deep water, and the
        /// bed came out 12 per cent bright.
        ///
        /// So this interpolates the way the sampler does — sixteen positions inside every texel,
        /// wrapping at the edge as the tile's own sampler does — rather than reading texel centres.
        ///
        /// Integer levels, because a fraction of one is a blend of two of them and the shader's own
        /// blend of two shares stands in for that.
        TEST_F(RtxWavePassTest, theResolvedCurvatureTableIsWhatASamplerFinds)
        {
            const Device& device = getDevice();
            CommandPool pool(device);
            const WavePass waves(device, pool, Testing::getShaderDirectory());

            synthesise(waves, pool, 0.0f);

            const WaveCurvature& carried = waves.getMoments();

            for (std::size_t cascade = 0; cascade < Shaders::WAVE_CASCADES; ++cascade)
            {
                const Image& curvature = waves.getCurvature(cascade);

                for (std::uint32_t level = 0; level < curvature.getMipLevels(); ++level)
                {
                    const std::vector<float> held = Testing::readHalves(pool, curvature, level);
                    const std::uint32_t width = curvature.getWidthAt(level);

                    constexpr std::uint32_t inside = 4;
                    const auto traceAt = [&](std::uint32_t x, std::uint32_t y) {
                        const std::size_t at = (std::size_t{ y % width } * width + x % width) * 4;
                        return double{ held[at] } + double{ held[at + 1] };
                    };

                    double squares = 0.0;
                    for (std::uint32_t y = 0; y < width; ++y)
                        for (std::uint32_t x = 0; x < width; ++x)
                            for (std::uint32_t down = 0; down < inside; ++down)
                                for (std::uint32_t across = 0; across < inside; ++across)
                                {
                                    const double u = (across + 0.5) / inside;
                                    const double v = (down + 0.5) / inside;
                                    const double tapped = (1.0 - u) * (1.0 - v) * traceAt(x, y)
                                        + u * (1.0 - v) * traceAt(x + 1, y) + (1.0 - u) * v * traceAt(x, y + 1)
                                        + u * v * traceAt(x + 1, y + 1);

                                    squares += tapped * tapped;
                                }

                    const double taps = static_cast<double>(width) * width * inside * inside;
                    const float measured = static_cast<float>(squares / taps);
                    const float wanted = carried.mWhole * carried.mResolved[cascade * Shaders::WAVE_LEVELS + level];

                    // A twentieth of the finest level, which is what a half-float chain leaves of a
                    // quantity that has been squared: the trace is rounded to eleven bits at every
                    // level, and the square doubles what that costs.
                    EXPECT_NEAR(
                        measured, wanted, 0.05f * carried.mWhole * carried.mResolved[cascade * Shaders::WAVE_LEVELS])
                        << "cascade " << cascade << " at level " << level;
                }
            }
        }

        /// The sea moves, and it moves at the speed its own dispersion relation gives.
        ///
        /// **A field that only turned its phases would translate rather than beat.** Every entry
        /// turns at its own `omega`, so a second later the surface is a different surface and not the
        /// same one shifted — which is what a correlation well under one says and what a rigid
        /// translation could not.
        TEST_F(RtxWavePassTest, theSurfaceAtAnotherMomentIsAnotherSurface)
        {
            const Device& device = getDevice();
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
