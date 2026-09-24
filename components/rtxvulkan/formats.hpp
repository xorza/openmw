#pragma once

#include <vulkan/vulkan_core.h>

#include <components/rtx/contract.hpp>
#include <components/rtx/shaders/storageformat.h>
#include <components/rtx/texturedata.hpp>

namespace Rtx
{
    /// The one place a `TextureFormat` becomes Vulkan's. A colour's cases are sRGB, because the files
    /// hold display-encoded bytes and the hardware converts them in the filter; data's are UNORM.
    /// Ends the process for a format `describeImage` refuses, because one arriving here is a
    /// contract broken and not a file.
    VkFormat toVulkanFormat(TextureFormat format);

    /// The one place a shader's declared layout becomes Vulkan's: the format of the Vulkan
    /// specification's "Compatibility Between SPIR-V Image Formats and Vulkan Formats" table.
    /// Constant, so a format a pass names at namespace scope stays one.
    constexpr VkFormat toVulkanFormat(const Shaders::StorageFormat format)
    {
        switch (format)
        {
            case Shaders::StorageFormat::Rgba8:
                return VK_FORMAT_R8G8B8A8_UNORM;
            case Shaders::StorageFormat::R16:
                return VK_FORMAT_R16_UNORM;
            case Shaders::StorageFormat::R16f:
                return VK_FORMAT_R16_SFLOAT;
            case Shaders::StorageFormat::R32f:
                return VK_FORMAT_R32_SFLOAT;
            case Shaders::StorageFormat::Rg16f:
                return VK_FORMAT_R16G16_SFLOAT;
            case Shaders::StorageFormat::Rg32f:
                return VK_FORMAT_R32G32_SFLOAT;
            case Shaders::StorageFormat::Rgba16f:
                return VK_FORMAT_R16G16B16A16_SFLOAT;
            case Shaders::StorageFormat::Rgba32f:
                return VK_FORMAT_R32G32B32A32_SFLOAT;
        }

        broken("a storage format with no Vulkan format");
    }
}
