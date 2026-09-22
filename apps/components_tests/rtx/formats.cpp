#include <array>
#include <utility>

#include <gtest/gtest.h>

#include <vulkan/vulkan_core.h>

#include <components/rtx/shaders/storageformat.h>
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
    }
}
