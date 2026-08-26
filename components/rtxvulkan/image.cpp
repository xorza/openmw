#include "image.hpp"

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
        /// **A read-back has to know, and only the format does.** Every image here is uncompressed
        /// and single-plane, so this is the whole of the question — and a format that reaches here
        /// unlisted is a new one somebody added without saying how large it is.
        std::uint32_t texelBytesOf(VkFormat format)
        {
            switch (format)
            {
                case VK_FORMAT_R8_UNORM:
                    return 1;
                case VK_FORMAT_R8G8B8A8_UNORM:
                    return 4;
                case VK_FORMAT_R32_SFLOAT:
                    return 4;
                case VK_FORMAT_R16G16B16A16_SFLOAT:
                    return 8;
                case VK_FORMAT_R32G32_SFLOAT:
                    return 8;
                case VK_FORMAT_R32G32B32A32_SFLOAT:
                    return 16;
                default:
                    throw Error("no texel size is recorded for this image format");
            }
        }
    }

    namespace
    {
        /// What an exportable or imported image has to be created with, so that its layout is one
        /// both sides agree on rather than one this driver chose privately.
        constexpr VkExternalMemoryImageCreateInfo sShareable{
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
        };
    }

    Image::Image(const Device& device, std::uint32_t width, std::uint32_t height, VkFormat format,
        VkImageUsageFlags usage, std::string_view name, Sharing sharing)
        : mDevice(device)
        , mWidth(width)
        , mHeight(height)
        , mFormat(format)
        , mUsage(usage)
        , mTexelBytes(texelBytesOf(format))
    {
        const bool exportable = sharing == Sharing::Exportable;

        const VkImageCreateInfo create{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = exportable ? &sShareable : nullptr,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = format,
            .extent = { width, height, 1 },
            .mipLevels = 1,
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
        mMemory = DeviceMemory(device, requirements.size, requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false, exportable);
        checkVk(vkBindImageMemory(device.getHandle(), mHandle, mMemory.getHandle(), 0), "vkBindImageMemory");

        const VkImageViewCreateInfo view{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = mHandle,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = format,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        checkVk(vkCreateImageView(device.getHandle(), &view, nullptr, &mView), "vkCreateImageView");

        device.setName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<std::uint64_t>(mHandle), name);
        device.setName(VK_OBJECT_TYPE_IMAGE_VIEW, reinterpret_cast<std::uint64_t>(mView), name);
    }

    Image::Image(const Device& device, std::uint32_t width, std::uint32_t height, VkFormat format,
        VkImageUsageFlags usage, std::string_view name, int memory, VkDeviceSize bytes)
        : mDevice(device)
        , mWidth(width)
        , mHeight(height)
        , mFormat(format)
        , mUsage(usage)
        , mTexelBytes(texelBytesOf(format))
    {
        // **Created shareable as well as bound to shared memory.** The flag is what makes the
        // driver lay the image out the way both sides agreed rather than however it would privately,
        // and an import onto an image created without it aliases the right bytes in the wrong order.
        const VkImageCreateInfo create{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = &sShareable,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = format,
            .extent = { width, height, 1 },
            .mipLevels = 1,
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
        mMemory = DeviceMemory(device, bytes, requirements.memoryTypeBits, memory);
        checkVk(vkBindImageMemory(device.getHandle(), mHandle, mMemory.getHandle(), 0), "vkBindImageMemory");

        const VkImageViewCreateInfo view{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = mHandle,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = format,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        checkVk(vkCreateImageView(device.getHandle(), &view, nullptr, &mView), "vkCreateImageView");

        device.setName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<std::uint64_t>(mHandle), name);
        device.setName(VK_OBJECT_TYPE_IMAGE_VIEW, reinterpret_cast<std::uint64_t>(mView), name);
    }

    Image::~Image()
    {
        if (mView != VK_NULL_HANDLE)
            vkDestroyImageView(mDevice.getHandle(), mView, nullptr);
        if (mHandle != VK_NULL_HANDLE)
            vkDestroyImage(mDevice.getHandle(), mHandle, nullptr);
    }

    void Image::transition(VkCommandBuffer commands, VkImageLayout from, VkImageLayout to,
        VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
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
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };

        const VkDependencyInfo dependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &barrier,
        };
        vkCmdPipelineBarrier2(commands, &dependency);
    }

    void Image::read(CommandPool& pool, VkImageLayout layout, std::vector<std::uint8_t>& pixels) const
    {
        const VkDeviceSize bytes = VkDeviceSize{ mWidth } * mHeight * mTexelBytes;
        const Buffer staging(mDevice, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        pool.submitAndWait([&](VkCommandBuffer commands) {
            transition(commands, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);

            const VkBufferImageCopy region{
                .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .imageExtent = { mWidth, mHeight, 1 },
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
        const void* mapped = staging.map();
        std::memcpy(pixels.data(), mapped, bytes);
        staging.unmap();
    }
}
