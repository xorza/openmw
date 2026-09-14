#include "image.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>

#include <components/rtx/error.hpp>

#include "barriers.hpp"
#include "buffer.hpp"
#include "commands.hpp"
#include "device.hpp"
#include "result.hpp"

namespace Rtx
{
    namespace
    {
        /// How many bytes one texel takes, for the formats this renderer makes images in, because
        /// a read-back has to know. Nought for a block format, whose texels have no size of their
        /// own and which nothing reads back.
        std::uint32_t texelBytesOf(VkFormat format)
        {
            switch (format)
            {
                case VK_FORMAT_R8_UNORM:
                    return 1;
                case VK_FORMAT_R8G8_UNORM:
                    return 2;
                case VK_FORMAT_R16_UNORM:
                case VK_FORMAT_R16_SFLOAT:
                    return 2;
                case VK_FORMAT_R8G8B8A8_UNORM:
                case VK_FORMAT_R8G8B8A8_SRGB:
                case VK_FORMAT_B8G8R8A8_SRGB:
                    return 4;
                case VK_FORMAT_R16G16_SFLOAT:
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
        : mDevice(&device)
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
        mHandle = Owned<VkImage, vkDestroyImage>::make(device.getHandle(), vkCreateImage, create, "vkCreateImage");

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device.getHandle(), mHandle.get(), &requirements);
        mMemory = device.getMemory().take(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, Tiling::Optimal);
        checkVk(vkBindImageMemory(device.getHandle(), mHandle.get(), mMemory.getHandle(), mMemory.getOffset()),
            "vkBindImageMemory");

        const VkImageViewCreateInfo view{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = mHandle.get(),
            .viewType = volume ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D,
            .format = format,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, 1 },
        };
        mView = Owned<VkImageView, vkDestroyImageView>::make(
            device.getHandle(), vkCreateImageView, view, "vkCreateImageView");

        // Only where something will write through it. A storage descriptor is what this second
        // view exists for, and an image without the usage bit can have none — a chain that is only
        // ever sampled would be paying for a view nothing may name.
        if (mipLevels > 1 && (usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0)
        {
            VkImageViewCreateInfo first = view;
            first.subresourceRange.levelCount = 1;
            mStorageView = Owned<VkImageView, vkDestroyImageView>::make(
                device.getHandle(), vkCreateImageView, first, "vkCreateImageView");
            device.setName(mStorageView.get(), name);
        }

        device.setName(mHandle.get(), name);
        device.setName(mView.get(), name);
    }

    void Image::transition(VkCommandBuffer commands, const ImageUse& from, const ImageUse& to) const
    {
        transitionLevels(commands, 0, mMipLevels, from, to);
    }

    VkImageMemoryBarrier2 Image::describeTransition(const ImageUse& from, const ImageUse& to) const
    {
        return describeLevels(0, mMipLevels, from, to);
    }

    void Image::clear(
        VkCommandBuffer commands, const ImageUse& from, const VkClearColorValue& colour, const ImageUse& to) const
    {
        assert((mUsage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0 && "a clear of an image not made to be written");

        transition(commands, from, Use::sClearWrite);

        const VkImageSubresourceRange whole{ VK_IMAGE_ASPECT_COLOR_BIT, 0, mMipLevels, 0, 1 };
        vkCmdClearColorImage(commands, mHandle.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1, &whole);

        transition(commands, Use::sClearWrite, to);
    }

    void Image::copyTo(
        VkCommandBuffer commands, const Image& into, const VkImageLayout intoLayout, const VkExtent2D extent) const
    {
        assert(extent.width <= mWidth && extent.height <= mHeight && "a copy of more than this image holds");
        assert(extent.width <= into.getWidth() && extent.height <= into.getHeight()
            && "a copy of more than the target holds");

        const VkImageCopy region{
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .extent = { extent.width, extent.height, 1 },
        };
        vkCmdCopyImage(
            commands, mHandle.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, into.getHandle(), intoLayout, 1, &region);
    }

    VkImageMemoryBarrier2 Image::describeLevels(
        std::uint32_t base, std::uint32_t count, const ImageUse& from, const ImageUse& to) const
    {
        return imageBarrier(mHandle.get(), base, count, from, to);
    }

    void Image::transitionLevels(VkCommandBuffer commands, std::uint32_t base, std::uint32_t count,
        const ImageUse& from, const ImageUse& to) const
    {
        Barriers barriers(commands);
        barriers.add(describeLevels(base, count, from, to));
        barriers.flush();
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
        transitionLevels(commands, 0, 1, Use::sComputeWrite, Use::sBlitRead);
        transitionLevels(commands, 1, mMipLevels - 1, Use::sDiscardForBlit, Use::sBlitWrite);

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
            vkCmdBlitImage(commands, mHandle.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mHandle.get(),
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR);

            // What was just written is the next blit's source, which is the whole of the ordering:
            // every level is written once and read once, by the step after it.
            transitionLevels(commands, level, 1, Use::sBlitWrite, Use::sBlitRead);

            width = halfWidth;
            height = halfHeight;
        }

        // Both stages, because the wave tiles are what has a chain: the fog volume samples them as
        // a dispatch and the trace as a launch.
        transitionLevels(commands, 0, mMipLevels, Use::sBlitRead, Use::sShaderSample);
    }

    VkDeviceSize Image::getReadBytes(const std::uint32_t level) const
    {
        assert(level < mMipLevels && "a level this image does not hold");
        assert(mDepth == 1 && "a read hands back one slice, and a volume has more than one");
        assert(mTexelBytes > 0 && "a read of an image whose texels come in blocks");

        return VkDeviceSize{ getWidthAt(level) } * getHeightAt(level) * mTexelBytes;
    }

    void Image::recordRead(const VkCommandBuffer commands, const ImageUse& before, const ImageUse& after,
        const Buffer& into, const std::uint32_t level) const
    {
        assert(into.getSize() >= getReadBytes(level) && "a read into a buffer too short for the level");

        transition(commands, before, Use::sCopyRead);

        const VkBufferImageCopy region{
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 },
            .imageExtent = { getWidthAt(level), getHeightAt(level), 1 },
        };
        vkCmdCopyImageToBuffer(
            commands, mHandle.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, into.getHandle(), 1, &region);

        into.orderForHostRead(commands);
        transition(commands, Use::sCopyRead, after);
    }

    void Image::read(
        CommandPool& pool, VkImageLayout layout, std::vector<std::uint8_t>& pixels, std::uint32_t level) const
    {
        const VkDeviceSize bytes = getReadBytes(level);
        const Buffer staging = Buffer::staging(*mDevice, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, "read back");

        // Back where it was found. Reading an image is not a change to it, and a caller that
        // has to know a read moved it is one that will forget: the GUI's own table is sampled
        // straight after the global map takes a copy of a tile out of it.
        pool.submitAndWait([&](VkCommandBuffer commands) {
            recordRead(commands, ImageUse{ layout, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT },
                ImageUse{ layout, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT },
                staging, level);
        });

        pixels.resize(bytes);
        std::memcpy(pixels.data(), staging.map(), bytes);
    }

    Image makeStandIn(const Device& device, CommandPool& pool, const VkFormat format, const VkImageUsageFlags usage,
        const std::string_view name)
    {
        assert((usage == VK_IMAGE_USAGE_STORAGE_BIT || usage == VK_IMAGE_USAGE_SAMPLED_BIT)
            && "a stand-in is read one way or the other, never both");

        Image made(device, 1, 1, format, usage, name);

        const VkAccessFlags2 read = usage == VK_IMAGE_USAGE_STORAGE_BIT ? VK_ACCESS_2_SHADER_STORAGE_READ_BIT
                                                                        : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        pool.submitAndWait([&](VkCommandBuffer commands) {
            made.transition(commands, Use::sUndefined,
                ImageUse{ VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, read });
        });

        return made;
    }
}
