#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <vulkan/vulkan_core.h>

#include <components/rtx/shaders/digest.h>
#include <components/rtxvulkan/buffer.hpp>
#include <components/rtxvulkan/commands.hpp>
#include <components/rtxvulkan/digestpass.hpp>
#include <components/rtxvulkan/image.hpp>
#include <components/rtxvulkan/imageuse.hpp>

#include "harness.hpp"

namespace Rtx
{
    namespace
    {
        using Lanes = std::array<std::uint32_t, Shaders::DIGEST_LANES>;

        /// One texel as the shader sees it after the load: four words of bits, with the channels
        /// the format lacks filled the way an image load fills them — nought, and one for alpha.
        struct Texel
        {
            std::uint32_t mR = 0;
            std::uint32_t mG = 0;
            std::uint32_t mB = 0;
            std::uint32_t mA = std::bit_cast<std::uint32_t>(1.0f);
        };

        /// The shader's four words, folded on the host in raster order — any order would do, which
        /// is the point of a sum and an exclusive-or.
        Lanes fold(const std::vector<Texel>& texels, const std::uint32_t width)
        {
            Lanes lanes{};
            for (std::size_t at = 0; at < texels.size(); ++at)
            {
                const Texel& texel = texels[at];
                const auto x = static_cast<std::uint32_t>(at % width);
                const auto y = static_cast<std::uint32_t>(at / width);

                const std::uint32_t sum
                    = Shaders::digestTexel(texel.mR, texel.mG, texel.mB, texel.mA, x, y, Shaders::DIGEST_SEED_SUM);
                const std::uint32_t xor_
                    = Shaders::digestTexel(texel.mR, texel.mG, texel.mB, texel.mA, x, y, Shaders::DIGEST_SEED_XOR);

                lanes[0] += sum;
                lanes[1] ^= sum;
                lanes[2] += xor_;
                lanes[3] ^= xor_;
            }

            return lanes;
        }

        /// A pattern no two texels share and no row repeats, with signs and a nought in it.
        float valueAt(const std::uint32_t x, const std::uint32_t y, const std::uint32_t channel)
        {
            const float signed_ = (x + y + channel) % 3 == 0 ? -1.0f : 1.0f;
            if ((x * 7 + y * 3 + channel) % 11 == 0)
                return 0.0f;

            return signed_
                * (static_cast<float>(x) * 0.125f + static_cast<float>(y) * 3.5f + static_cast<float>(channel));
        }

        struct RtxDigestPassTest : Testing::DeviceTest
        {
            /// Digests `images` in one record, from where `from` left them, and hands back every
            /// image's lanes. The pass takes the frame's count of images, so the last one given
            /// stands in for the rest.
            std::vector<Lanes> digest(const std::span<Image* const> images, const ImageUse& from = Use::sTextureSample)
            {
                Device& device = getDevice();
                const DigestPass pass(device, Testing::getShaderDirectory());
                const Buffer lanes
                    = Buffer::readBack(device, sizeof(std::uint32_t) * Shaders::DIGEST_LANES * Shaders::DIGEST_IMAGES,
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, "digest test lanes");

                std::array<const Image*, Shaders::DIGEST_IMAGES> digested{};
                for (std::size_t at = 0; at < digested.size(); ++at)
                    digested[at] = images[std::min(at, images.size() - 1)];

                Batch batch(getPool());
                for (Image* const image : images)
                    image->transition(batch.getCommands(), from, Use::sComputeRead);
                pass.record(batch.getCommands(), digested, lanes, nullptr);
                batch.flush();

                std::vector<Lanes> read(Shaders::DIGEST_IMAGES);
                const auto* const words = static_cast<const std::uint32_t*>(lanes.map());
                for (std::size_t image = 0; image < read.size(); ++image)
                    for (std::size_t lane = 0; lane < Shaders::DIGEST_LANES; ++lane)
                        read[image][lane] = words[image * Shaders::DIGEST_LANES + lane];

                return read;
            }

            /// An image of `channels` floats a texel, uploaded and left where a sampler expects it.
            Image upload(const std::uint32_t width, const std::uint32_t height, const std::uint32_t channels,
                const VkFormat format, const std::vector<float>& values, const std::string_view name)
            {
                EXPECT_EQ(values.size(), std::size_t{ width } * height * channels);

                Image image(getDevice(), width, height, format,
                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, name);

                std::vector<VkBufferImageCopy> regions{ VkBufferImageCopy{
                    .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                    .imageExtent = { width, height, 1 },
                } };

                Batch batch(getPool());
                uploadImage(batch, image, std::as_bytes(std::span(values)), regions);
                batch.flush();

                return image;
            }
        };

        TEST_F(RtxDigestPassTest, theDeviceDigestIsTheHostsToTheBit)
        {
            // Wider and taller than a workgroup and not a multiple of one, so the overhang folds
            // nothing and the second workgroup's texels land where the host puts them.
            constexpr std::uint32_t width = 37;
            constexpr std::uint32_t height = 21;

            std::vector<float> values;
            std::vector<Texel> texels;
            for (std::uint32_t y = 0; y < height; ++y)
                for (std::uint32_t x = 0; x < width; ++x)
                {
                    Texel texel;
                    texel.mR = std::bit_cast<std::uint32_t>(valueAt(x, y, 0));
                    texel.mG = std::bit_cast<std::uint32_t>(valueAt(x, y, 1));
                    texel.mB = std::bit_cast<std::uint32_t>(valueAt(x, y, 2));
                    texel.mA = std::bit_cast<std::uint32_t>(valueAt(x, y, 3));
                    texels.push_back(texel);
                    for (std::uint32_t channel = 0; channel < 4; ++channel)
                        values.push_back(valueAt(x, y, channel));
                }

            Image image = upload(width, height, 4, VK_FORMAT_R32G32B32A32_SFLOAT, values, "digest test rgba");
            const Lanes expected = fold(texels, width);

            std::array<Image*, 1> once{ &image };
            const std::vector<Lanes> first = digest(once);
            ASSERT_EQ(first.size(), Shaders::DIGEST_IMAGES);
            for (std::size_t slot = 0; slot < first.size(); ++slot)
                for (std::size_t lane = 0; lane < Shaders::DIGEST_LANES; ++lane)
                    EXPECT_EQ(first[slot][lane], expected[lane]) << "image " << slot << " lane " << lane;

            EXPECT_NE(expected[0], 0u) << "a digest of something is not the digest of nothing";
            EXPECT_NE(expected[0], expected[2]) << "the two seeds are two mixes";

            EXPECT_EQ(digest(once, Use::sComputeRead), first) << "the same image twice is the same digest";
        }

        TEST_F(RtxDigestPassTest, oneTexelMovesTheDigestAndOnlyItsImage)
        {
            constexpr std::uint32_t width = 20;
            constexpr std::uint32_t height = 18;

            std::vector<float> values(std::size_t{ width } * height * 4);
            for (std::uint32_t at = 0; at < values.size(); ++at)
                values[at] = static_cast<float>(at % 13) * 0.5f;

            Image same = upload(width, height, 4, VK_FORMAT_R32G32B32A32_SFLOAT, values, "digest test same");
            Image other = upload(width, height, 4, VK_FORMAT_R32G32B32A32_SFLOAT, values, "digest test other");

            // The last texel's blue, by one unit in the last place.
            values[values.size() - 2]
                = std::bit_cast<float>(std::bit_cast<std::uint32_t>(values[values.size() - 2]) + 1);
            Image moved = upload(width, height, 4, VK_FORMAT_R32G32B32A32_SFLOAT, values, "digest test moved");

            std::array<Image*, 3> three{ &same, &other, &moved };
            const std::vector<Lanes> lanes = digest(three);
            ASSERT_EQ(lanes.size(), Shaders::DIGEST_IMAGES);

            EXPECT_EQ(lanes[0], lanes[1]) << "two images of the same bits digest the same in different lanes";
            EXPECT_NE(lanes[0], lanes[2]) << "one bit of one texel is a different image";
            for (std::size_t lane = 0; lane < Shaders::DIGEST_LANES; ++lane)
                EXPECT_NE(lanes[0][lane], lanes[2][lane]) << "every lane sees the texel, lane " << lane;
        }

        TEST_F(RtxDigestPassTest, aFormatShortOfChannelsIsReadAsTheLoadFillsIt)
        {
            constexpr std::uint32_t width = 5;
            constexpr std::uint32_t height = 3;

            std::vector<float> values;
            std::vector<Texel> texels;
            for (std::uint32_t y = 0; y < height; ++y)
                for (std::uint32_t x = 0; x < width; ++x)
                {
                    Texel texel;
                    texel.mR = std::bit_cast<std::uint32_t>(valueAt(x, y, 0));
                    texel.mG = std::bit_cast<std::uint32_t>(valueAt(x, y, 1));
                    texels.push_back(texel);
                    values.push_back(valueAt(x, y, 0));
                    values.push_back(valueAt(x, y, 1));
                }

            Image image = upload(width, height, 2, VK_FORMAT_R32G32_SFLOAT, values, "digest test rg");

            std::array<Image*, 1> once{ &image };
            const std::vector<Lanes> lanes = digest(once);
            ASSERT_EQ(lanes.size(), Shaders::DIGEST_IMAGES);
            EXPECT_EQ(lanes[0], fold(texels, width)) << "blue reads as nought and alpha as one";
        }
    }
}
