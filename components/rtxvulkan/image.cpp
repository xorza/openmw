#include "image.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>

#include <components/rtx/error.hpp>

#include "buffer.hpp"
#include "commands.hpp"
#include "device.hpp"
#include "result.hpp"

namespace Rtx
{
    namespace
    {
        /// How many bytes one texel takes, for the formats this renderer makes images in.
        ///
        /// **A read-back has to know, and only the format does.** Nothing here is multi-planar, so
        /// this is the whole of the question — and a format that reaches here unlisted is a new one
        /// somebody added without saying how large it is.
        ///
        /// **Nought for a block format**, whose texels have no size of their own: what a content
        /// texture holds is four-by-four blocks, and nothing reads one of those back. `read` is
        /// where that is said.
        std::uint32_t texelBytesOf(VkFormat format)
        {
            switch (format)
            {
                case VK_FORMAT_R8_UNORM:
                    return 1;
                case VK_FORMAT_R8G8_UNORM:
                    return 2;
                case VK_FORMAT_R16_SFLOAT:
                    return 2;
                case VK_FORMAT_R8G8B8A8_UNORM:
                case VK_FORMAT_R8G8B8A8_SRGB:
                case VK_FORMAT_B8G8R8A8_SRGB:
                    return 4;
                case VK_FORMAT_R32_SFLOAT:
                    return 4;
                case VK_FORMAT_R16G16B16A16_SFLOAT:
                    return 8;
                case VK_FORMAT_R32G32_SFLOAT:
                    return 8;
                case VK_FORMAT_R32G32B32A32_SFLOAT:
                    return 16;

                case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
                case VK_FORMAT_BC2_SRGB_BLOCK:
                case VK_FORMAT_BC3_SRGB_BLOCK:
                    return 0;

                default:
                    throw Error("no texel size is recorded for this image format");
            }
        }
    }

    Image::Image(const Device& device, std::uint32_t width, std::uint32_t height, VkFormat format,
        VkImageUsageFlags usage, std::string_view name, std::uint32_t mipLevels, std::uint32_t depth)
        : mDevice(device)
        , mWidth(width)
        , mHeight(height)
        , mDepth(depth)
        , mFormat(format)
        , mUsage(usage)
        , mMipLevels(mipLevels)
        , mTexelBytes(texelBytesOf(format))
    {
        assert(mipLevels >= 1 && "an image holds its own full level at least");
        assert(depth >= 1 && "an image holds one slice at least");

        const bool volume = depth > 1;

        const VkImageCreateInfo create{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
            .format = format,
            .extent = { width, height, depth },
            .mipLevels = mipLevels,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        checkVk(vkCreateImage(device.getHandle(), &create, nullptr, &mHandle), "vkCreateImage");

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device.getHandle(), mHandle, &requirements);
        mMemory = DeviceMemory(
            device, requirements.size, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false);
        checkVk(vkBindImageMemory(device.getHandle(), mHandle, mMemory.getHandle(), 0), "vkBindImageMemory");

        const VkImageViewCreateInfo view{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = mHandle,
            .viewType = volume ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D,
            .format = format,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, 1 },
        };
        checkVk(vkCreateImageView(device.getHandle(), &view, nullptr, &mView), "vkCreateImageView");

        // **Only where something will write through it.** A storage descriptor is what this second
        // view exists for, and an image without the usage bit can have none — a chain that is only
        // ever sampled would be paying for a view nothing may name.
        if (mipLevels > 1 && (usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0)
        {
            VkImageViewCreateInfo first = view;
            first.subresourceRange.levelCount = 1;
            checkVk(vkCreateImageView(device.getHandle(), &first, nullptr, &mStorageView), "vkCreateImageView");
            device.setName(VK_OBJECT_TYPE_IMAGE_VIEW, reinterpret_cast<std::uint64_t>(mStorageView), name);
        }

        device.setName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<std::uint64_t>(mHandle), name);
        device.setName(VK_OBJECT_TYPE_IMAGE_VIEW, reinterpret_cast<std::uint64_t>(mView), name);
    }

    Image::~Image()
    {
        if (mStorageView != VK_NULL_HANDLE)
            vkDestroyImageView(mDevice.getHandle(), mStorageView, nullptr);
        if (mView != VK_NULL_HANDLE)
            vkDestroyImageView(mDevice.getHandle(), mView, nullptr);
        if (mHandle != VK_NULL_HANDLE)
            vkDestroyImage(mDevice.getHandle(), mHandle, nullptr);
    }

    void Image::transition(VkCommandBuffer commands, VkImageLayout from, VkImageLayout to,
        VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
        VkAccessFlags2 dstAccess) const
    {
        transitionLevels(commands, 0, mMipLevels, from, to, srcStage, srcAccess, dstStage, dstAccess);
    }

    void Image::transitionLevels(VkCommandBuffer commands, std::uint32_t base, std::uint32_t count, VkImageLayout from,
        VkImageLayout to, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
        VkAccessFlags2 dstAccess) const
    {
        const VkImageMemoryBarrier2 barrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = srcStage,
            .srcAccessMask = srcAccess,
            .dstStageMask = dstStage,
            .dstAccessMask = dstAccess,
            .oldLayout = from,
            .newLayout = to,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = mHandle,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, base, count, 0, 1 },
        };

        const VkDependencyInfo dependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &barrier,
        };
        vkCmdPipelineBarrier2(commands, &dependency);
    }

    void Image::buildMips(VkCommandBuffer commands) const
    {
        assert((mUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0 && "a chain reads the level above it");
        assert((mUsage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0 && "a chain writes the level below it");
        assert(mDepth == 1 && "a volume's chain is uploaded rather than blitted");

        if (mMipLevels <= 1)
            return;

        // The written level becomes the first source; the rest hold whatever the last frame left,
        // which every blit below overwrites whole.
        transitionLevels(commands, 0, 1, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT);
        transitionLevels(commands, 1, mMipLevels - 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_BLIT_BIT, 0, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

        std::uint32_t width = mWidth;
        std::uint32_t height = mHeight;

        for (std::uint32_t level = 1; level < mMipLevels; ++level)
        {
            // A level never falls below one texel, which is what makes the last of them the whole
            // image's own mean.
            const std::uint32_t halfWidth = std::max(width / 2, 1u);
            const std::uint32_t halfHeight = std::max(height / 2, 1u);

            const VkImageBlit region{
                .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1 },
                .srcOffsets = { {}, { static_cast<std::int32_t>(width), static_cast<std::int32_t>(height), 1 } },
                .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 },
                .dstOffsets
                = { {}, { static_cast<std::int32_t>(halfWidth), static_cast<std::int32_t>(halfHeight), 1 } },
            };
            vkCmdBlitImage(commands, mHandle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mHandle,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR);

            // What was just written is the next blit's source, which is the whole of the ordering:
            // every level is written once and read once, by the step after it.
            transitionLevels(commands, level, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);

            width = halfWidth;
            height = halfHeight;
        }

        transitionLevels(commands, 0, mMipLevels, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }

    void Image::read(
        CommandPool& pool, VkImageLayout layout, std::vector<std::uint8_t>& pixels, std::uint32_t level) const
    {
        assert(level < mMipLevels && "a level this image does not hold");
        assert(mDepth == 1 && "a read hands back one slice, and a volume has more than one");
        assert(mTexelBytes > 0 && "a read of an image whose texels come in blocks");

        const std::uint32_t width = getWidthAt(level);
        const std::uint32_t height = getHeightAt(level);
        const VkDeviceSize bytes = VkDeviceSize{ width } * height * mTexelBytes;
        const Buffer staging = Buffer::staging(mDevice, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);

        pool.submitAndWait([&](VkCommandBuffer commands) {
            transition(commands, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);

            const VkBufferImageCopy region{
                .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 },
                .imageExtent = { width, height, 1 },
            };
            vkCmdCopyImageToBuffer(
                commands, mHandle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.getHandle(), 1, &region);

            // **Back where it was found.** Reading an image is not a change to it, and a caller that
            // has to know a read moved it is one that will forget: the GUI's own table is sampled
            // straight after the global map takes a copy of a tile out of it.
            transition(commands, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        });

        pixels.resize(bytes);
        std::memcpy(pixels.data(), staging.map(), bytes);
    }
}
