#include "image.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <utility>

#include <components/rtx/contract.hpp>

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
                case VK_FORMAT_B8G8R8A8_UNORM:
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
                case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
                case VK_FORMAT_BC2_SRGB_BLOCK:
                case VK_FORMAT_BC2_UNORM_BLOCK:
                case VK_FORMAT_BC3_SRGB_BLOCK:
                case VK_FORMAT_BC3_UNORM_BLOCK:
                case VK_FORMAT_BC5_UNORM_BLOCK:
                    return 0;

                default:
                    broken("no texel size is recorded for this image format");
            }
        }
    }

    Image::Image(const Device& device, std::uint32_t width, std::uint32_t height, VkFormat format,
        VkImageUsageFlags usage, std::string_view name, std::uint32_t mipLevels, std::uint32_t depth,
        VkFormat storageFormat)
        : Image(Unbound{}, device, width, height, format, usage, name, mipLevels, depth, storageFormat)
    {
        bind(device.getMemory().take(mHandle.get(), VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT), name);
    }

    Result<Image, std::string_view> Image::tryMake(const MemoryUse use, const Device& device, std::uint32_t width,
        std::uint32_t height, VkFormat format, VkImageUsageFlags usage, std::string_view name, std::uint32_t mipLevels,
        std::uint32_t depth, VkFormat storageFormat)
    {
        Image made(Unbound{}, device, width, height, format, usage, name, mipLevels, depth, storageFormat);
        Result<DeviceMemory, std::string_view> memory
            = device.getMemory().tryTake(made.mHandle.get(), VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, use);
        if (!memory.isOk())
            return Err{ memory.error() };

        made.bind(std::move(memory.value()), name);
        return made;
    }

    Image::Image(Unbound, const Device& device, std::uint32_t width, std::uint32_t height, VkFormat format,
        VkImageUsageFlags usage, std::string_view name, std::uint32_t mipLevels, std::uint32_t depth,
        VkFormat storageFormat)
        : mDevice(&device)
        , mWidth(width)
        , mHeight(height)
        , mDepth(depth)
        , mFormat(format)
        , mUsage(usage)
        , mMipLevels(mipLevels)
        , mTexelBytes(texelBytesOf(format))
        , mStorageFormat(storageFormat)
    {
        assert(mipLevels >= 1 && "an image holds its own full level at least");
        assert(depth >= 1 && "an image holds one slice at least");

        const bool volume = depth > 1;

        // A second format is a second view of the same bits, which the image has to be created
        // able to give: the list is what lets the driver keep the image's own layout for both.
        // **Extended usage, because the storage usage is the other format's.** An `SRGB` format
        // has no storage feature, so an image of it asking for storage is refused outright — the
        // extended-usage flag has the usage checked against every format of the list instead,
        // and the view in the image's own format below then has to say it carries no storage.
        const bool twoFormats = storageFormat != VK_FORMAT_UNDEFINED && storageFormat != format;
        const std::array<VkFormat, 2> formats{ format, storageFormat };
        const VkImageFormatListCreateInfo list{
            .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
            .viewFormatCount = 2,
            .pViewFormats = formats.data(),
        };

        const VkImageCreateInfo create{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = twoFormats ? &list : nullptr,
            .flags = twoFormats
                ? VkImageCreateFlags{ VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT }
                : VkImageCreateFlags{},
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
        device.setName(mHandle.get(), name);
    }

    void Image::bind(DeviceMemory&& memory, std::string_view name)
    {
        const Device& device = *mDevice;
        const bool volume = mDepth > 1;
        const bool twoFormats = mStorageFormat != VK_FORMAT_UNDEFINED && mStorageFormat != mFormat;

        mMemory = std::move(memory);
        checkVk(vkBindImageMemory(device.getHandle(), mHandle.get(), mMemory.getHandle(), mMemory.getOffset()),
            "vkBindImageMemory");

        const VkImageViewUsageCreateInfo sampledOnly{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
            .usage = mUsage & ~VkImageUsageFlags{ VK_IMAGE_USAGE_STORAGE_BIT },
        };
        const VkImageViewCreateInfo view{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = twoFormats ? &sampledOnly : nullptr,
            .image = mHandle.get(),
            .viewType = volume ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D,
            .format = mFormat,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mMipLevels, 0, 1 },
        };
        mView = Owned<VkImageView, vkDestroyImageView>::make(
            device.getHandle(), vkCreateImageView, view, "vkCreateImageView");

        // Only where something will write through them. A storage descriptor is what these views
        // exist for, and an image without the usage bit can have none — a chain that is only ever
        // sampled would be paying for views nothing may name.
        if ((mMipLevels > 1 || twoFormats) && (mUsage & VK_IMAGE_USAGE_STORAGE_BIT) != 0)
        {
            mLevelViews.reserve(mMipLevels);
            for (std::uint32_t level = 0; level < mMipLevels; ++level)
            {
                VkImageViewCreateInfo one = view;
                one.pNext = nullptr;
                one.format = twoFormats ? mStorageFormat : mFormat;
                one.subresourceRange.baseMipLevel = level;
                one.subresourceRange.levelCount = 1;
                mLevelViews.push_back(Owned<VkImageView, vkDestroyImageView>::make(
                    device.getHandle(), vkCreateImageView, one, "vkCreateImageView"));
                device.setName(mLevelViews.back().get(), name);
            }
        }

        device.setName(mView.get(), name);
    }

    Image::~Image()
    {
        assert(mayDestroy() && "an image destroyed while a submit may still read it; bury it");
    }

    Image& Image::operator=(Image&& other) noexcept
    {
        if (this != &other)
        {
            assert(mayDestroy() && "an image written over while a submit may still read it; replace it");

            mDevice = other.mDevice;
            mRead = other.mRead;
            mHandle = std::move(other.mHandle);
            mView = std::move(other.mView);
            mLevelViews = std::move(other.mLevelViews);
            mMemory = std::move(other.mMemory);
            mWidth = other.mWidth;
            mHeight = other.mHeight;
            mDepth = other.mDepth;
            mFormat = other.mFormat;
            mUsage = other.mUsage;
            mMipLevels = other.mMipLevels;
            mTexelBytes = other.mTexelBytes;
            mStorageFormat = other.mStorageFormat;
        }

        return *this;
    }

    bool Image::mayDestroy() const
    {
        return isEmpty() || isIdle();
    }

    bool Image::isIdle() const
    {
        return mDevice == nullptr || mRead.isIdle(*mDevice);
    }

    void Image::waitIdle(const char* const what) const
    {
        if (mDevice != nullptr)
            mRead.waitIdle(*mDevice, what);
    }

    void Image::nameForNext() const
    {
        assert(!isEmpty() && "a submit named on an image nobody made");
        mRead.nameFor(mDevice->getTimeline().getNext());
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
        assert(!isEmpty() && "a clear of an image nobody made");

        assert((mUsage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0 && "a clear of an image not made to be written");

        transition(commands, from, Use::sClearWrite);

        const VkImageSubresourceRange whole{ VK_IMAGE_ASPECT_COLOR_BIT, 0, mMipLevels, 0, 1 };
        vkCmdClearColorImage(commands, mHandle.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1, &whole);

        transition(commands, Use::sClearWrite, to);
    }

    void Image::copyTo(
        VkCommandBuffer commands, const Image& into, const VkImageLayout intoLayout, const VkExtent2D extent) const
    {
        assert(!isEmpty() && "a copy out of an image nobody made");

        assert(extent.width <= mWidth && extent.height <= mHeight && "a copy of more than this image holds");
        assert(extent.width <= into.getWidth() && extent.height <= into.getHeight()
            && "a copy of more than the target holds");

        // Both ends, because a copy takes handles and names nothing on its own.
        nameForNext();
        into.nameForNext();

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
        assert(!isEmpty() && "a barrier on an image nobody made");

        // A barrier is the one thing every use in a command buffer records around itself, so
        // a description of one is where a use of the handle names the image.
        nameForNext();
        return imageBarrier(mHandle.get(), base, count, from, to);
    }

    void Image::transitionLevels(VkCommandBuffer commands, std::uint32_t base, std::uint32_t count,
        const ImageUse& from, const ImageUse& to) const
    {
        Barriers barriers(commands);
        barriers.add(describeLevels(base, count, from, to));
        barriers.flush();
    }

    void Image::buildMips(VkCommandBuffer commands, const std::span<const Image* const> images)
    {
        // The written level becomes the first source; the rest hold whatever the last frame left,
        // which every blit below overwrites whole. One command for every chain, so the queue drains
        // the dispatches once.
        std::uint32_t deepest = 1;
        Barriers opened(commands);
        for (const Image* image : images)
        {
            assert(!image->isEmpty() && "a chain built on an image nobody made");
            assert((image->mUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0 && "a chain reads the level above it");
            assert((image->mUsage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0 && "a chain writes the level below it");
            assert(image->mDepth == 1 && "a volume's chain is uploaded rather than blitted");

            if (image->mMipLevels <= 1)
                continue;

            deepest = std::max(deepest, image->mMipLevels);
            opened.add(image->describeLevels(0, 1, Use::sComputeWrite, Use::sBlitRead));
            opened.add(image->describeLevels(1, image->mMipLevels - 1, Use::sUndefined, Use::sBlitWrite));
        }
        opened.flush();

        for (std::uint32_t level = 1; level < deepest; ++level)
        {
            Barriers written(commands);
            for (const Image* image : images)
            {
                if (level >= image->mMipLevels)
                    continue;

                // A level never falls below one texel, which is what makes the last of them the
                // whole image's own mean.
                const VkImageBlit region{
                    .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1 },
                    .srcOffsets = { {},
                        { static_cast<std::int32_t>(image->getWidthAt(level - 1)),
                            static_cast<std::int32_t>(image->getHeightAt(level - 1)), 1 } },
                    .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 },
                    .dstOffsets = { {},
                        { static_cast<std::int32_t>(image->getWidthAt(level)),
                            static_cast<std::int32_t>(image->getHeightAt(level)), 1 } },
                };
                vkCmdBlitImage(commands, image->mHandle.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    image->mHandle.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR);

                // What was just written is the next blit's source, which is the whole of the
                // ordering: every level is written once and read once, by the step after it.
                written.add(image->describeLevels(level, 1, Use::sBlitWrite, Use::sBlitRead));
            }
            written.flush();
        }

        // Both stages, because the wave tiles are what has a chain: the fog volume samples them as
        // a dispatch and the trace as a launch.
        Barriers sampled(commands);
        for (const Image* image : images)
            if (image->mMipLevels > 1)
                sampled.add(image->describeLevels(0, image->mMipLevels, Use::sBlitRead, Use::sShaderSample));
        sampled.flush();
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
        assert(!isEmpty() && "a read of an image nobody made");

        assert(into.getSize() >= getReadBytes(level) && "a read into a buffer too short for the level");

        transition(commands, before, Use::sCopyRead);
        into.nameForNext();

        const VkBufferImageCopy region{
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 },
            .imageExtent = { getWidthAt(level), getHeightAt(level), 1 },
        };
        vkCmdCopyImageToBuffer(
            commands, mHandle.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, into.getHandle(), 1, &region);

        into.orderForHostRead(commands);
        transition(commands, Use::sCopyRead, after);
    }

    void Image::read(VkImageLayout layout, std::vector<std::uint8_t>& pixels, std::uint32_t level) const
    {
        const VkDeviceSize bytes = getReadBytes(level);
        const Buffer landing = Buffer::readBack(*mDevice, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, "read back");

        // Back where it was found. Reading an image is not a change to it, and a caller that
        // has to know a read moved it is one that will forget: the GUI's own table is sampled
        // straight after the global map takes a copy of a tile out of it.
        mDevice->getPool().submitAndWait([&](VkCommandBuffer commands) {
            recordRead(commands, ImageUse{ layout, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT },
                ImageUse{ layout, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT },
                landing, level);
        });

        pixels.resize(bytes);
        std::memcpy(pixels.data(), landing.map(), bytes);
    }

    Image makeStandIn(
        const Device& device, const VkFormat format, const VkImageUsageFlags usage, const std::string_view name)
    {
        assert((usage == VK_IMAGE_USAGE_STORAGE_BIT || usage == VK_IMAGE_USAGE_SAMPLED_BIT)
            && "a stand-in is read one way or the other, never both");

        Image made(device, 1, 1, format, usage, name);

        const VkAccessFlags2 read = usage == VK_IMAGE_USAGE_STORAGE_BIT ? VK_ACCESS_2_SHADER_STORAGE_READ_BIT
                                                                        : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        device.getPool().submitAndWait([&](VkCommandBuffer commands) {
            made.transition(commands, Use::sUndefined,
                ImageUse{ VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, read });
        });

        return made;
    }
}
