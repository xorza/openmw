#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <vulkan/vulkan_core.h>

#include <components/rtx/shaders/scene.h>
#include <components/rtx/shadingmap.hpp>
#include <components/rtx/texturedata.hpp>
#include <components/rtxvulkan/commands.hpp>
#include <components/rtxvulkan/device.hpp>
#include <components/rtxvulkan/handles.hpp>
#include <components/rtxvulkan/image.hpp>
#include <components/rtxvulkan/memory.hpp>
#include <components/rtxvulkan/shadingpass.hpp>
#include <components/rtxvulkan/texture.hpp>

#include "harness.hpp"
#include "testtexture.hpp"

namespace Rtx
{
    namespace
    {
        struct RtxShadingPassTest : Testing::DeviceTest
        {
            /// Uploads `data` as the array does, from level `first` on, runs the pass over it again
            /// into a map of the test's own, and hands that map back as the device stores it: one
            /// unorm16 a cell, row by row. The test's own map and not `Texture`'s, because a map
            /// the trace samples is never copied back and carries no usage for it.
            std::vector<std::uint16_t> mapOf(const TextureData& data, std::string_view name, std::uint32_t first = 0)
            {
                Device& device = getDevice();
                const Testing::TexturePassSet passes(device);
                const Sampler sampler = makeContentSampler(device, "shading test");

                const Image map(device, Shaders::SHADING_EXTENT, Shaders::SHADING_EXTENT, VK_FORMAT_R16_UNORM,
                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                    "shading test map");

                Batch upload(getPool());
                std::vector<VkBufferImageCopy> regions;
                const Texture source = std::move(Texture::fromFile(
                    device, upload, passes.mPasses, sampler.get(), data, first, name, regions, MemoryUse::Essential)
                                                     .value());
                passes.mShading.record(upload.getCommands(), source.getImage(), sampler.get(), map, data);
                upload.flush();

                std::vector<std::uint8_t> bytes;
                map.read(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, bytes);
                EXPECT_EQ(bytes.size(), ShadingMap::sCells * sizeof(std::uint16_t));

                std::vector<std::uint16_t> read(ShadingMap::sCells);
                for (std::size_t cell = 0; cell < read.size() && cell * 2 + 1 < bytes.size(); ++cell)
                    read[cell] = static_cast<std::uint16_t>(bytes[cell * 2] | bytes[cell * 2 + 1] << 8);
                return read;
            }
        };

        /// The device's estimate is the host's, cell for cell.
        ///
        /// `Testing::paintTwoTones` says what the texture is and why its size: 1.501 across the
        /// middle half and the floor, 0.5, at the sides. **In the linear format to two steps of
        /// 65535**, which is the two summing in different orders: the arithmetic is the same
        /// arithmetic. **Display-encoded to thirty-two steps**, which is the device's decode of a
        /// byte against the host's exact curve: the sampler's curve is a table the specification
        /// allows a tolerance on, measured at twenty-one steps here, and every cell of the map
        /// carries it because the mean does.
        TEST_F(RtxShadingPassTest, theDeviceEstimateIsTheHosts)
        {
            const auto compare = [&](TextureFormat format, int steps, std::string_view name) {
                const Testing::TestTexture painted = Testing::paintTwoTones(32, 96, format);
                const ShadingMap host(painted.mData);
                const std::vector<std::uint16_t> device = mapOf(painted.mData, name);
                EXPECT_EQ(device.size(), ShadingMap::sCells);

                for (std::size_t cell = 0; cell < device.size(); ++cell)
                {
                    const int expected = encodeShading(host.getValues()[cell]);
                    EXPECT_NEAR(int{ device[cell] }, expected, steps) << name << " at cell " << cell;
                }

                return device;
            };

            compare(TextureFormat::Rgba8Unorm, 2, "linear two tones");
            const std::vector<std::uint16_t> encoded = compare(TextureFormat::Rgba8Srgb, 32, "encoded two tones");

            // And the shape, so the comparison is known to be of a map worth comparing.
            constexpr std::size_t middle = 16 * ShadingMap::sExtent + 16;
            constexpr std::size_t side = 16 * ShadingMap::sExtent + 2;
            ASSERT_EQ(encoded.size(), ShadingMap::sCells);
            EXPECT_NEAR(decodeShading(encoded[middle]), 1.501f, 0.002f);
            EXPECT_NEAR(decodeShading(encoded[side]), 0.5f, 0.002f);
        }

        /// A texture held to a smaller side is estimated over the level it stands from, as the host
        /// estimates that level on its own.
        ///
        /// **The image's size and not the file's.** The two tones at 128 across and again at 64 —
        /// bright over columns 32 to 96 and then 16 to 48, which is the first level box-filtered
        /// exactly, every boundary being on an even column. Stood from the second level the image is
        /// 64 across, and an estimate over the file's 128 reads three quarters of its texels past
        /// the image. Linear, so the two sum the same arithmetic, to the two steps the first test
        /// allows.
        TEST_F(RtxShadingPassTest, aTextureHeldToASmallerSideIsEstimatedOverTheLevelItStandsFrom)
        {
            constexpr std::uint32_t halfExtent = Testing::sTwoTonesExtent / 2;
            std::vector<std::uint8_t> halved(std::size_t{ halfExtent } * halfExtent * 4, 255);
            for (std::uint32_t y = 0; y < halfExtent; ++y)
                for (std::uint32_t x = 0; x < halfExtent; ++x)
                    for (std::size_t channel = 0; channel < 3; ++channel)
                        halved[(std::size_t{ y } * halfExtent + x) * 4 + channel] = x >= 16 && x < 48 ? 255 : 156;

            Testing::TestTexture half;
            Testing::paintFlat(half, halfExtent, halved, "half");

            const Testing::TestTexture full = Testing::paintTwoTones(32, 96, TextureFormat::Rgba8Unorm);
            Testing::TestTexture both;
            both.mBytes = full.mBytes;
            both.mBytes.insert(both.mBytes.end(), halved.begin(), halved.end());
            both.mLevels = { full.mLevels.front(),
                MipLevel{ static_cast<std::uint32_t>(full.mBytes.size()), halfExtent, halfExtent } };
            both.describe(Testing::sTwoTonesExtent, Testing::sTwoTonesExtent, "two levels");

            const ShadingMap host(half.mData);
            const std::vector<std::uint16_t> device = mapOf(both.mData, "two levels from the second", 1);
            ASSERT_EQ(device.size(), ShadingMap::sCells);
            for (std::size_t cell = 0; cell < device.size(); ++cell)
                EXPECT_NEAR(int{ device[cell] }, encodeShading(host.getValues()[cell]), 2) << "at cell " << cell;
        }

        /// A BC1 hole is not a colour.
        ///
        /// One BC1 block with its endpoints ascending — three colours and a hole — both endpoints
        /// white, every even index nought and every odd one three: eight white texels and eight
        /// holes. Every sampled cell is white, so the map is neutral throughout, 21845; a decode
        /// that counted the holes as black would put a dark cell beside a bright one and the map
        /// would swing to its clamps. That a texture flagged neutral is cleared to the same value
        /// without a dispatch is `Texture`'s, and `aTexturesPaintedLightIsDividedBackOutOfItsAlbedo`
        /// draws with one.
        TEST_F(RtxShadingPassTest, aHoleIsNoColour)
        {
            // 0xFFFF as both endpoints, then indices 0, 3, 0, 3 ... — 0b11001100 a byte.
            Testing::TestTexture block;
            block.mBytes.assign({ 0xFF, 0xFF, 0xFF, 0xFF, 0xCC, 0xCC, 0xCC, 0xCC });
            block.mLevels.assign(1, MipLevel{ 0, 4, 4 });
            block.describe(4, 4, "holes", TextureFormat::Bc1RgbaSrgb);

            const std::vector<std::uint16_t> estimated = mapOf(block.mData, "holes");
            ASSERT_EQ(estimated.size(), ShadingMap::sCells);
            for (std::size_t cell = 0; cell < ShadingMap::sCells; cell += 37)
                EXPECT_EQ(int{ estimated[cell] }, 21845) << "at cell " << cell;
        }
    }
}
