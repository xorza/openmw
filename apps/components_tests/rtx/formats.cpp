#include <array>
#include <cstddef>
#include <utility>

#include <gtest/gtest.h>

#include <vulkan/vulkan_core.h>

#include <components/rtx/shaders/storageformat.h>
#include <components/rtx/texels.hpp>
#include <components/rtx/texturedata.hpp>
#include <components/rtxvulkan/formats.hpp>

namespace Rtx
{
    namespace
    {
        using Shaders::StorageFormat;

        /// **A layout a shader declares is created as the format the specification pairs it with.**
        /// A view in any other format is undefined behaviour that the layers only warn about, so
        /// this mapping is the whole of what keeps an image and its declaration agreeing.
        ///
        /// Copied by hand from the Vulkan specification's "Compatibility Between SPIR-V Image
        /// Formats and Vulkan Formats" table, one row per qualifier `storageformat.h` spells.
        TEST(RtxFormatsTest, aStorageFormatIsTheOneTheSpecificationPairsItsQualifierWith)
        {
            constexpr std::array<std::pair<StorageFormat, VkFormat>, 8> sTable{ {
                { StorageFormat::Rgba8, VK_FORMAT_R8G8B8A8_UNORM },
                { StorageFormat::R16, VK_FORMAT_R16_UNORM },
                { StorageFormat::R16f, VK_FORMAT_R16_SFLOAT },
                { StorageFormat::R32f, VK_FORMAT_R32_SFLOAT },
                { StorageFormat::Rg16f, VK_FORMAT_R16G16_SFLOAT },
                { StorageFormat::Rg32f, VK_FORMAT_R32G32_SFLOAT },
                { StorageFormat::Rgba16f, VK_FORMAT_R16G16B16A16_SFLOAT },
                { StorageFormat::Rgba32f, VK_FORMAT_R32G32B32A32_SFLOAT },
            } };

            for (const auto& [format, expected] : sTable)
                EXPECT_EQ(toVulkanFormat(format), expected) << "layout " << static_cast<int>(format);
        }

        /// **Every format this uploads is the block or order its file holds, with the curve where it
        /// is a colour and without it where it is data.** A normal map read through the sRGB curve
        /// bends every direction toward one corner, and nothing downstream could tell.
        TEST(RtxFormatsTest, anUploadedFormatIsItsFilesBlockWithTheCurveOnlyForAColour)
        {
            constexpr std::array<std::pair<TextureFormat, VkFormat>, 11> sTable{ {
                { TextureFormat::Bc1RgbaSrgb, VK_FORMAT_BC1_RGBA_SRGB_BLOCK },
                { TextureFormat::Bc2Srgb, VK_FORMAT_BC2_SRGB_BLOCK },
                { TextureFormat::Bc3Srgb, VK_FORMAT_BC3_SRGB_BLOCK },
                { TextureFormat::Rgba8Unorm, VK_FORMAT_R8G8B8A8_UNORM },
                { TextureFormat::Rgba8Srgb, VK_FORMAT_R8G8B8A8_SRGB },
                { TextureFormat::Bgra8Srgb, VK_FORMAT_B8G8R8A8_SRGB },
                { TextureFormat::Bc1RgbaUnorm, VK_FORMAT_BC1_RGBA_UNORM_BLOCK },
                { TextureFormat::Bc2Unorm, VK_FORMAT_BC2_UNORM_BLOCK },
                { TextureFormat::Bc3Unorm, VK_FORMAT_BC3_UNORM_BLOCK },
                { TextureFormat::Bgra8Unorm, VK_FORMAT_B8G8R8A8_UNORM },
                { TextureFormat::Bc5Unorm, VK_FORMAT_BC5_UNORM_BLOCK },
            } };

            std::size_t uploadable = 0;
            for (std::size_t at = 0; at < sTextureFormatCount; ++at)
                uploadable += isUploadable(static_cast<TextureFormat>(at)) ? 1 : 0;
            EXPECT_EQ(uploadable, sTable.size()) << "an uploadable format no row above names";

            for (const auto& [format, expected] : sTable)
                EXPECT_EQ(toVulkanFormat(format), expected) << nameOf(format);
        }
    }
}
