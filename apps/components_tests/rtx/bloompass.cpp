#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtx/shaders/bloom.h>
#include <components/rtxvulkan/bloompass.hpp>
#include <components/rtxvulkan/buffer.hpp>
#include <components/rtxvulkan/commands.hpp>
#include <components/rtxvulkan/image.hpp>

#include "harness.hpp"

namespace Rtx
{
    namespace
    {
        /// Big enough for every level `BLOOM_LEVELS` asks for: six halvings of 320 by 256 end at
        /// 5 by 4, which is the last one no narrower than `BLOOM_NARROWEST`.
        constexpr std::uint32_t sWidth = 320;
        constexpr std::uint32_t sHeight = 256;

        /// What the pass is handed, made the way the renderer makes its own frame: storage because
        /// the composite writes it, sampled because the first halving reads it, and both transfer
        /// bits so a test can fill it.
        std::unique_ptr<Image> makeFrame(const Device& device, std::uint32_t width, std::uint32_t height)
        {
            return std::make_unique<Image>(device, width, height, VK_FORMAT_R32G32B32A32_SFLOAT,
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                    | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                "test-bloom-frame");
        }

        /// Puts `pixels` in the image and leaves it in `VK_IMAGE_LAYOUT_GENERAL`, where the frame
        /// path leaves it.
        void paint(CommandPool& pool, const Device& device, const Image& image, std::span<const float> pixels)
        {
            const Buffer staging = Buffer::hostWritten(device, pixels.size_bytes(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
            staging.writeAt(0, pixels);

            pool.submitAndWait([&](VkCommandBuffer commands) {
                image.transition(commands, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT);

                const VkBufferImageCopy region{
                    .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                    .imageExtent = { image.getWidth(), image.getHeight(), 1 },
                };
                vkCmdCopyBufferToImage(
                    commands, staging.getHandle(), image.getHandle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

                image.transition(commands, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            });
        }

        /// The red channel at a texel, which is the one every frame below varies.
        float redAt(std::span<const float> values, std::uint32_t width, std::uint32_t x, std::uint32_t y)
        {
            return values[(std::size_t{ y } * width + x) * 4];
        }

        /// The pass, a frame the size it was built for, and the pool that drives both.
        ///
        /// Bundled because the three have to agree about the extent: a pyramid built for one frame
        /// and run over another is what `BloomPass::record` asserts against, and a test that got it
        /// wrong would abort rather than fail.
        struct Bloomed
        {
            CommandPool mPool;
            BloomPass mBloom;
            std::unique_ptr<Image> mFrame;

            Bloomed(const Device& device, std::uint32_t width, std::uint32_t height)
                : mPool(device)
                , mBloom(device, Testing::getShaderDirectory())
                , mFrame(makeFrame(device, width, height))
            {
                mBloom.resize(width, height);
            }

            /// What the pyramid's finest level holds after one run, or nothing where there is no
            /// pyramid.
            std::vector<float> over(const Device& device, std::span<const float> pixels)
            {
                paint(mPool, device, *mFrame, pixels);
                mPool.submitAndWait([&](VkCommandBuffer commands) { mBloom.record(commands, *mFrame); });

                const Image* pyramid = mBloom.getPyramid();
                return pyramid != nullptr ? Testing::readHalves(mPool, *pyramid) : std::vector<float>();
            }
        };

        /// A frame with nothing to spread comes out of the pyramid as it went in.
        ///
        /// **Both kernels are partitions of one**, so a flat frame halved is the same flat frame at
        /// every level, and mixing a value with itself at any weight is that value. A weight table
        /// that summed to anything else, a mix taken the wrong way round, or a level read before the
        /// dispatch that filled it had finished, all move this.
        TEST(RtxBloomPassTest, aFlatFrameComesBackFlat)
        {
            std::string reason;
            Testing::Harness* harness = Testing::getHarness(reason);
            if (harness == nullptr)
                GTEST_SKIP() << reason;

            const Device& device = *harness->mDevice;

            Bloomed run(device, sWidth, sHeight);
            EXPECT_EQ(run.mBloom.getLevelCount(), Shaders::BLOOM_LEVELS) << "a frame with room for every halving";

            // Quarters, so the half floats the levels are kept in hold each of them exactly and what
            // is compared below is the arithmetic rather than the rounding.
            std::vector<float> flat(std::size_t{ sWidth } * sHeight * 4);
            for (std::size_t at = 0; at < flat.size(); at += 4)
            {
                flat[at] = 0.25f;
                flat[at + 1] = 0.5f;
                flat[at + 2] = 0.75f;
                flat[at + 3] = 1.0f;
            }

            const std::vector<float> spread = run.over(device, flat);

            ASSERT_EQ(spread.size(), std::size_t{ sWidth / 2 } * (sHeight / 2) * 4);
            for (std::size_t at = 0; at < spread.size(); at += 4)
            {
                ASSERT_NEAR(spread[at], 0.25f, 1.0e-3f) << "at " << at;
                ASSERT_NEAR(spread[at + 1], 0.5f, 1.0e-3f) << "at " << at;
                ASSERT_NEAR(spread[at + 2], 0.75f, 1.0e-3f) << "at " << at;
            }
        }

        /// A frame with one bright square in it comes out spread, and falls away with distance.
        ///
        /// **The pyramid is an average of the frame**, so nothing in it may be brighter than the
        /// brightest thing that went in, the square's own texels have to come out dimmer than the
        /// square was, and a texel that was black has to come out lit. Between those the glow must
        /// fall away, which is the whole of what six halvings and five tents are for.
        TEST(RtxBloomPassTest, oneBrightSquareSpreadsAndFallsAwayWithDistance)
        {
            std::string reason;
            Testing::Harness* harness = Testing::getHarness(reason);
            if (harness == nullptr)
                GTEST_SKIP() << reason;

            const Device& device = *harness->mDevice;

            constexpr float sBright = 8.0f;
            constexpr std::uint32_t sBlock = 16;

            std::vector<float> square(std::size_t{ sWidth } * sHeight * 4);
            for (std::size_t at = 3; at < square.size(); at += 4)
                square[at] = 1.0f;

            const std::uint32_t left = sWidth / 2 - sBlock / 2;
            const std::uint32_t top = sHeight / 2 - sBlock / 2;
            for (std::uint32_t y = top; y < top + sBlock; ++y)
                for (std::uint32_t x = left; x < left + sBlock; ++x)
                    square[(std::size_t{ y } * sWidth + x) * 4] = sBright;

            Bloomed run(device, sWidth, sHeight);
            const std::vector<float> spread = run.over(device, square);

            // The level is half the frame across, so the square's own eight texels of it are centred
            // on 80 by 64 and its left edge is at 76.
            const std::uint32_t across = sWidth / 2;
            const std::uint32_t middle = sHeight / 4;
            const std::uint32_t edge = left / 2;

            for (std::size_t at = 0; at < spread.size(); at += 4)
                ASSERT_LE(spread[at], sBright) << "an average of the frame cannot exceed it, at " << at;

            const float centre = redAt(spread, across, across / 2, middle);
            EXPECT_LT(centre, sBright) << "a square that spread nothing is a bloom that did nothing";
            EXPECT_GT(centre, 0.0f);

            // And it falls away: four readings out from the square's edge, each dimmer than the one
            // inside it. A tent that lost its texel size would be flat across these.
            float last = redAt(spread, across, edge - 1, middle);
            EXPECT_GT(last, 0.0f) << "a texel that was black is lit";

            for (const std::uint32_t out : { 4u, 12u, 28u, 60u })
            {
                const float here = redAt(spread, across, edge - 1 - out, middle);
                EXPECT_LT(here, last) << "at " << out << " texels out";
                EXPECT_GT(here, 0.0f) << "and the widest level still reaches there";
                last = here;
            }
        }

        /// A frame too small to halve is one the pass builds no pyramid for.
        ///
        /// The levels are counted down from `BLOOM_LEVELS` and stop at `BLOOM_NARROWEST`, so a
        /// thumbnail gets a narrower pyramid and something smaller than one texel of it gets none —
        /// which the display pass reads as no lens rather than as a sampled stand-in.
        TEST(RtxBloomPassTest, aFrameTooSmallToHalveGetsNoPyramid)
        {
            std::string reason;
            Testing::Harness* harness = Testing::getHarness(reason);
            if (harness == nullptr)
                GTEST_SKIP() << reason;

            const Device& device = *harness->mDevice;

            // 40 by 32 halves to 20, 10 and 5 across, and to 16, 8 and 4 down — three levels, where
            // the fourth would be 2 high.
            BloomPass counting(device, Testing::getShaderDirectory());
            counting.resize(40, 32);
            EXPECT_EQ(counting.getLevelCount(), 3u);
            EXPECT_NE(counting.getPyramid(), nullptr);

            counting.resize(sWidth, sHeight);
            EXPECT_EQ(counting.getLevelCount(), Shaders::BLOOM_LEVELS) << "and it grows back";

            counting.resize(6, 6);
            EXPECT_EQ(counting.getLevelCount(), 0u) << "three across is under the narrowest level";
            EXPECT_EQ(counting.getPyramid(), nullptr);
        }
    }
}
