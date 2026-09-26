#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <vulkan/vulkan_core.h>

#include <components/rtx/alphaimage.hpp>
#include <components/rtx/texturedata.hpp>
#include <components/rtxvulkan/commands.hpp>
#include <components/rtxvulkan/device.hpp>
#include <components/rtxvulkan/handles.hpp>
#include <components/rtxvulkan/image.hpp>
#include <components/rtxvulkan/memory.hpp>
#include <components/rtxvulkan/spritelightpass.hpp>
#include <components/rtxvulkan/texture.hpp>

#include "support/device/harness.hpp"
#include "support/device/texturepasses.hpp"
#include "support/spritelightbake.hpp"
#include "support/testtexture.hpp"

namespace Rtx
{
    namespace
    {
        struct RtxSpriteLightPassTest : Testing::DeviceTest
        {
            /// Uploads `sprite` as the array does, bakes it on the device into a chain of the
            /// test's own, and hands every level of the bake back as bytes, level by level and row
            /// by row, four a texel. The test's own chain and not `Texture`'s, because a bake the
            /// trace samples is never copied back and carries no usage for it.
            std::vector<std::vector<std::uint8_t>> bakeOf(const TextureData& sprite)
            {
                Device& device = getDevice();
                const Testing::TexturePassSet passes(device);
                const Sampler sampler = makeContentSampler(device, "sprite light test");

                const auto levels = static_cast<std::uint32_t>(sprite.mLevels.size());
                const Image bake(device, sprite.mWidth, sprite.mHeight, VK_FORMAT_R8G8B8A8_UNORM,
                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                    "sprite light test bake", levels);

                Batch upload(getPool());
                std::vector<VkBufferImageCopy> regions;
                const Texture source = std::move(Texture::fromFile(
                    device, upload, passes.mPasses, sampler.get(), sprite, 0, "sprite", regions, MemoryUse::Essential)
                                                     .value());
                passes.mBake.record(upload.getCommands(), source.getImage(), sampler.get(), bake);
                upload.flush();

                std::vector<std::vector<std::uint8_t>> read(levels);
                for (std::uint32_t level = 0; level < levels; ++level)
                    bake.read(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, read[level], level);
                return read;
            }
        };

        /// The device's bake is the host's, texel for texel and level for level, to a byte.
        ///
        /// `RtxSpriteLightBakeTest` says what the host's bake is; this says the device makes the same
        /// one, over a sprite of two levels whose alphas are stated outright: a four-by-four with a
        /// dense middle and a two-by-two below it. Every channel of every texel of every level is
        /// compared, within a byte, because `pow` on the device and `std::pow` on the host differ in
        /// the last place and a product of a few of them can cross a rounding boundary.
        TEST_F(RtxSpriteLightPassTest, theDeviceBakeIsTheHostsToAByte)
        {
            Testing::TestTexture sprite;
            Testing::addAlphaLevel(sprite, 4, 4, { 0, 64, 64, 0, 32, 255, 255, 32, 32, 255, 200, 32, 0, 64, 64, 0 });
            Testing::addAlphaLevel(sprite, 2, 2, { 96, 160, 128, 64 });

            const AlphaImage alpha(sprite.mData);
            const Testing::SpriteLightBake host(alpha);
            ASSERT_EQ(host.describe().mLevels.size(), 2u);

            const std::vector<std::vector<std::uint8_t>> device = bakeOf(sprite.mData);
            ASSERT_EQ(device.size(), 2u);

            for (std::uint32_t level = 0; level < 2; ++level)
            {
                const MipLevel& shape = sprite.mLevels[level];
                ASSERT_EQ(device[level].size(), std::size_t{ shape.mWidth } * shape.mHeight * 4) << "level " << level;

                for (std::uint32_t y = 0; y < shape.mHeight; ++y)
                    for (std::uint32_t x = 0; x < shape.mWidth; ++x)
                        for (std::uint32_t channel = 0; channel < 4; ++channel)
                        {
                            const std::size_t at = (std::size_t{ y } * shape.mWidth + x) * 4 + channel;
                            EXPECT_NEAR(int{ device[level][at] }, int{ host.at(level, x, y, channel) }, 1)
                                << "level " << level << " at " << x << ", " << y << " channel " << channel;
                        }
            }

            // And a value the doc of the host's test derives by hand, so the comparison is known to
            // be of a bake with shadows in it: the finest level's first row is `0, 64, 64, 0`, and
            // light from the high edge reaching its first texel crossed two of `(1 - 64/255)^(1/4)`
            // = 0.93030, which is 0.86546 and byte 221.
            EXPECT_NEAR(int{ device[0][0] }, 221, 1);
        }
    }
}
