#include "formats.hpp"

namespace Rtx
{
    VkFormat toVulkanFormat(TextureFormat format)
    {
        switch (format)
        {
            case TextureFormat::Bc1RgbaSrgb:
                return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
            case TextureFormat::Bc2Srgb:
                return VK_FORMAT_BC2_SRGB_BLOCK;
            case TextureFormat::Bc3Srgb:
                return VK_FORMAT_BC3_SRGB_BLOCK;
            case TextureFormat::Rgba8Unorm:
                return VK_FORMAT_R8G8B8A8_UNORM;
            case TextureFormat::Rgba8Srgb:
                return VK_FORMAT_R8G8B8A8_SRGB;
            case TextureFormat::Bgra8Srgb:
                return VK_FORMAT_B8G8R8A8_SRGB;
            case TextureFormat::Bc1RgbaUnorm:
                return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
            case TextureFormat::Bc2Unorm:
                return VK_FORMAT_BC2_UNORM_BLOCK;
            case TextureFormat::Bc3Unorm:
                return VK_FORMAT_BC3_UNORM_BLOCK;
            case TextureFormat::Bgra8Unorm:
                return VK_FORMAT_B8G8R8A8_UNORM;
            case TextureFormat::Bc5Unorm:
                return VK_FORMAT_BC5_UNORM_BLOCK;

            // Never uploaded: `describeImage` refuses them, so one arriving here is a contract
            // broken and not a file.
            case TextureFormat::Rgb8:
            case TextureFormat::Luminance:
            case TextureFormat::LuminanceAlpha:
            case TextureFormat::Unnamed:
                break;
        }

        // A format nothing above named: a new one that forgets a case lands here rather than
        // creating an image with a format nobody chose.
        broken("a texture format this renderer does not upload");
    }
}
