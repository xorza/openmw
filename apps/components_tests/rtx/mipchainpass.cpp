#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <vulkan/vulkan_core.h>

#include <components/rtx/mipchain.hpp>
#include <components/rtx/texturedata.hpp>
#include <components/rtxvulkan/commands.hpp>
#include <components/rtxvulkan/device.hpp>
#include <components/rtxvulkan/handles.hpp>
#include <components/rtxvulkan/image.hpp>
#include <components/rtxvulkan/memory.hpp>
#include <components/rtxvulkan/texture.hpp>

#include "harness.hpp"
#include "testtexture.hpp"

namespace Rtx
{
    namespace
    {
        /// One level painted from `bytes`, four a texel.
        void addLevel(Testing::TestTexture& texture, std::uint32_t width, std::uint32_t height,
            std::initializer_list<std::uint8_t> bytes)
        {
            texture.mLevels.push_back(MipLevel{ static_cast<std::uint32_t>(texture.mBytes.size()), width, height });
            texture.mBytes.insert(texture.mBytes.end(), bytes);
        }

        struct RtxMipChainPassTest : Testing::DeviceTest
        {
            /// Uploads `file`'s one level as the array does, as the bytes it holds in `stored` — the
            /// file's format with no curve under it — makes the chain on the device into an image
            /// of the test's own, and hands every level of it back as bytes, level by level and row
            /// by row, four a texel. The test's own image and not `Texture`'s, because a chain the
            /// trace samples is never copied back and carries no usage for it.
            std::vector<std::vector<std::uint8_t>> chainOf(
                const TextureData& file, std::string_view name, VkFormat stored = VK_FORMAT_R8G8B8A8_UNORM)
            {
                Device& device = getDevice();
                const Testing::TexturePassSet passes(device);
                const Sampler sampler = makeContentSampler(device, "mip chain test");

                const bool encoded = isSrgb(file.mFormat);
                const std::uint32_t levels = levelsTo1x1(file.mWidth, file.mHeight);
                const Image chain(device, file.mWidth, file.mHeight,
                    encoded ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM,
                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                    "mip chain test chain", levels, 1, encoded ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_UNDEFINED);

                Batch upload(getPool());
                Image source(device, file.mWidth, file.mHeight, stored,
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, name, 1);
                std::vector<VkBufferImageCopy> regions{ VkBufferImageCopy{
                    .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                    .imageExtent = { file.mWidth, file.mHeight, 1 },
                } };
                uploadImage(upload, source, file.mBytes, regions);
                passes.mChain.record(upload.getCommands(), source, sampler.get(), chain, encoded);
                upload.flush();

                std::vector<std::vector<std::uint8_t>> read(levels);
                for (std::uint32_t level = 0; level < levels; ++level)
                    chain.read(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, read[level], level);
                return read;
            }

            /// How many levels `Texture` stands `file` with, which says whether it made a chain.
            std::uint32_t levelsStood(TextureData file, std::string_view name)
            {
                file.mCompleteChain = MipChain::wantedFor(file);

                Device& device = getDevice();
                const Testing::TexturePassSet passes(device);
                const Sampler sampler = makeContentSampler(device, "mip chain test");

                Batch upload(getPool());
                std::vector<VkBufferImageCopy> regions;
                const Texture stood = std::move(Texture::fromFile(
                    device, upload, passes.mPasses, sampler.get(), file, 0, name, regions, MemoryUse::Essential)
                                                    .value());
                upload.flush();

                return stood.getImage().getMipLevels();
            }

            /// The device's chain against the host's, every channel of every texel of every level
            /// within a byte: the device decodes and encodes the curve in float where the host goes
            /// through a table, and a mean can land on a rounding boundary.
            void expectHosts(const TextureData& file, std::string_view name)
            {
                const MipChain host(file);
                ASSERT_FALSE(host.isEmpty()) << name;
                const TextureData built = host.describe();

                const std::vector<std::vector<std::uint8_t>> device = chainOf(file, name);
                ASSERT_EQ(device.size(), built.mLevels.size()) << name;

                for (std::size_t level = 0; level < device.size(); ++level)
                {
                    const MipLevel& shape = built.mLevels[level];
                    ASSERT_EQ(device[level].size(), std::size_t{ shape.mWidth } * shape.mHeight * 4)
                        << name << " level " << level;

                    for (std::size_t at = 0; at < device[level].size(); ++at)
                    {
                        const int expected = std::to_integer<int>(built.mBytes[shape.mOffset + at]);
                        EXPECT_NEAR(int{ device[level][at] }, expected, 1)
                            << name << " level " << level << " byte " << at;
                    }
                }
            }
        };

        /// The device's chain is the host's, over the three textures `RtxMipChainTest` derives its
        /// figures for by hand: four quads of four greys down to their mean, two white texels at
        /// full alpha beside two black at none, and a display-encoded pair averaged in light.
        ///
        /// The figures are stated again here, so the comparison is known to be of chains worth
        /// comparing: 85 is the mean of 100, 200, 0 and 40; 255 at 128 is white weighed by its
        /// alpha over black at none; 188 is half of white in light, where 128 would be half of it
        /// in bytes.
        TEST_F(RtxMipChainPassTest, theDeviceChainIsTheHostsToAByte)
        {
            Testing::TestTexture four;
            for (const std::uint8_t value : { 100, 200, 100, 200, 0, 40, 0, 40 })
                for (int twice = 0; twice < 2; ++twice)
                    for (const std::uint8_t byte : { value, value, value, std::uint8_t{ 255 } })
                        four.mBytes.push_back(byte);
            four.mLevels.push_back(MipLevel{ 0, 4, 4 });
            four.describe(4, 4, "four");
            expectHosts(four.mData, "four greys");

            const std::vector<std::vector<std::uint8_t>> greys = chainOf(four.mData, "four greys");
            ASSERT_EQ(greys.size(), 3u) << "4 by 4 runs down to one texel in three levels";
            EXPECT_EQ(int{ greys[0][0] }, 100) << "the finest level is the file's own";
            EXPECT_EQ(int{ greys[1][4] }, 200);
            EXPECT_NEAR(int{ greys[2][0] }, 85, 1) << "the mean of the four quads";
            EXPECT_EQ(int{ greys[2][3] }, 255) << "nothing was transparent, so nothing faded";

            Testing::TestTexture pair;
            addLevel(pair, 2, 2, { 255, 255, 255, 255, 255, 255, 255, 255, 0, 0, 0, 0, 0, 0, 0, 0 });
            pair.describe(2, 2, "pair");
            expectHosts(pair.mData, "white over transparent black");

            const std::vector<std::vector<std::uint8_t>> weighed = chainOf(pair.mData, "weighed");
            ASSERT_EQ(weighed.size(), 2u);
            EXPECT_EQ(int{ weighed[1][0] }, 255) << "the black the transparent texels stored was averaged in";
            EXPECT_NEAR(int{ weighed[1][3] }, 128, 1) << "half of it was painted";

            Testing::TestTexture encoded;
            addLevel(encoded, 2, 2, { 255, 255, 255, 255, 0, 0, 0, 255, 255, 255, 255, 255, 0, 0, 0, 255 });
            encoded.describe(2, 2, "encoded", TextureFormat::Rgba8Srgb);
            expectHosts(encoded.mData, "display-encoded pair");

            const std::vector<std::vector<std::uint8_t>> light = chainOf(encoded.mData, "encoded");
            ASSERT_EQ(light.size(), 2u);
            EXPECT_NEAR(int{ light[1][0] }, 188, 1) << "averaged in light and written back encoded";
        }

        /// A block-compressed file is fetched through the format's own decoder, texel for texel,
        /// and its holes are weighed as nothing.
        ///
        /// One BC1 block with its endpoints ascending — three colours and a hole — both endpoints
        /// white, every even index nought and every odd one three: eight white texels and eight
        /// holes, chequered. Every quad of the level below is two white at full alpha and two holes,
        /// so it is white at half alpha, and the one texel below that the same. Exact, because
        /// white is white however a decoder interpolates between two of it.
        TEST_F(RtxMipChainPassTest, aBlockCompressedFileIsFetchedDecodedAndItsHolesWeighNothing)
        {
            Testing::TestTexture block;
            block.mBytes.assign({ 0xFF, 0xFF, 0xFF, 0xFF, 0xCC, 0xCC, 0xCC, 0xCC });
            block.mLevels.assign(1, MipLevel{ 0, 4, 4 });
            block.describe(4, 4, "holes", TextureFormat::Bc1RgbaSrgb);

            const std::vector<std::vector<std::uint8_t>> chain
                = chainOf(block.mData, "holes", VK_FORMAT_BC1_RGBA_UNORM_BLOCK);
            ASSERT_EQ(chain.size(), 3u);
            ASSERT_EQ(chain[0].size(), 64u);

            EXPECT_EQ(int{ chain[0][0] }, 255) << "the first texel is white";
            EXPECT_EQ(int{ chain[0][3] }, 255);
            EXPECT_EQ(int{ chain[0][4] }, 0) << "and the second a hole, black at no alpha";
            EXPECT_EQ(int{ chain[0][7] }, 0);

            for (std::size_t texel = 0; texel < 4; ++texel)
            {
                EXPECT_EQ(int{ chain[1][texel * 4] }, 255) << "level one texel " << texel;
                EXPECT_NEAR(int{ chain[1][texel * 4 + 3] }, 128, 1);
            }
            EXPECT_EQ(int{ chain[2][0] }, 255);
            EXPECT_NEAR(int{ chain[2][3] }, 128, 1);
        }

        /// A texture stands with the chain its file did not carry, down to one texel, and with the
        /// chain it did carry as it came; a file of one texel has no chain to make. The builder
        /// marks a description by `MipChain::wantedFor`, and this says `Texture` keeps it.
        TEST_F(RtxMipChainPassTest, aTextureStandsWithTheChainItsFileDidNotCarry)
        {
            Testing::TestTexture single;
            addLevel(single, 4, 2,
                { 10, 20, 30, 255, 40, 50, 60, 255, 70, 80, 90, 255, 100, 110, 120, 255, 10, 20, 30, 255, 40, 50, 60,
                    255, 70, 80, 90, 255, 100, 110, 120, 255 });
            single.describe(4, 2, "single");
            EXPECT_EQ(levelsStood(single.mData, "single"), 3u) << "4 by 2 runs down to one texel in three levels";

            Testing::TestTexture whole;
            addLevel(whole, 2, 2, { 10, 20, 30, 255, 40, 50, 60, 255, 70, 80, 90, 255, 100, 110, 120, 255 });
            addLevel(whole, 1, 1, { 7, 8, 9, 255 });
            whole.describe(2, 2, "whole");
            EXPECT_EQ(levelsStood(whole.mData, "whole"), 2u) << "the file's own chain, as it came";

            Testing::TestTexture texel;
            addLevel(texel, 1, 1, { 1, 2, 3, 255 });
            texel.describe(1, 1, "texel");
            EXPECT_EQ(levelsStood(texel.mData, "texel"), 1u);
        }
    }
}
