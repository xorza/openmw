#pragma once

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "memory.hpp"

namespace Rtx
{
    class CommandPool;
    class Device;

    /// An image, its allocation and its view.
    class Image
    {
    public:
        /// @param name what a capture and a validation message call this image and its view.
        ///        Required, and not because every image deserves prose: they all used to be called
        ///        "target", so a report naming one said nothing about which it was.
        /// @param mipLevels how many halvings the image holds, including the full one. Levels
        ///        past the first hold nothing until `buildMips` fills them, and one is an image
        ///        with no chain at all.
        /// @param depth how many slices it holds. One is a 2D image, which is what everything a
        ///        camera writes or a screen reads is; more makes it a volume, and a chain over one
        ///        halves the third axis with the other two.
        Image(const Device& device, std::uint32_t width, std::uint32_t height, VkFormat format, VkImageUsageFlags usage,
            std::string_view name, std::uint32_t mipLevels = 1, std::uint32_t depth = 1);

        ~Image();

        Image(const Image&) = delete;
        Image& operator=(const Image&) = delete;

        VkImage getHandle() const { return mHandle; }

        /// The view a sampler reads, which covers every level.
        VkImageView getView() const { return mView; }

        /// The view a storage descriptor takes, which is the first level alone.
        ///
        /// **Vulkan will not let a storage image name a chain**, so an image with one is written
        /// through a second view of its own — and an image without one hands back the only view it
        /// has, so nothing that never asked for levels has anything to know about this.
        VkImageView getStorageView() const { return mStorageView != VK_NULL_HANDLE ? mStorageView : mView; }
        std::uint32_t getWidth() const { return mWidth; }
        std::uint32_t getHeight() const { return mHeight; }
        VkFormat getFormat() const { return mFormat; }
        std::uint32_t getMipLevels() const { return mMipLevels; }

        /// What this image was created able to do, which is not always what a reader assumes.
        ///
        /// **Kept so a mismatch can be asserted rather than looked at.** An image handed to
        /// something that samples it, without `VK_IMAGE_USAGE_SAMPLED_BIT`, reads as zero — no
        /// error, no validation message, just a black frame with nothing pointing at the cause.
        VkImageUsageFlags getUsage() const { return mUsage; }

        /// How many bytes one texel of this image's format occupies.
        std::uint32_t getTexelBytes() const { return mTexelBytes; }

        /// Moves every level of the image to `layout`, recording into `commands`.
        void transition(VkCommandBuffer commands, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 srcStage,
            VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) const;

        /// Fills every level below the first by halving the one above it, in `VK_FILTER_LINEAR`.
        ///
        /// **A box filter, which is what a moment wants.** A level of this chain is the mean of the
        /// four texels over it, so a channel carrying a square averages to a mean square and the
        /// difference of the two is the variance the level threw away. A wider or a sharper kernel
        /// would be a better picture and a worse statistic.
        ///
        /// Takes the whole image in `VK_IMAGE_LAYOUT_GENERAL` with the first level written and the
        /// rest holding nothing, and leaves it in `VK_IMAGE_LAYOUT_GENERAL` ordered against a
        /// sampled read. Needs `VK_IMAGE_USAGE_TRANSFER_SRC_BIT` and `VK_IMAGE_USAGE_TRANSFER_DST_BIT`.
        void buildMips(VkCommandBuffer commands) const;

        /// Copies one level to host memory, `getTexelBytes()` per pixel, tightly packed, row by row.
        ///
        /// **Left in the layout it was handed**, because reading an image is not a change to it and
        /// a caller that had to know a read moved it is one that would forget.
        ///
        /// Submits and waits, so it belongs to a screenshot rather than to a frame.
        void read(
            CommandPool& pool, VkImageLayout layout, std::vector<std::uint8_t>& pixels, std::uint32_t level = 0) const;

        /// How wide, how tall and how deep `level` is, which is the full size halved that many
        /// times and never below one texel.
        std::uint32_t getWidthAt(std::uint32_t level) const { return std::max(mWidth >> level, 1u); }
        std::uint32_t getHeightAt(std::uint32_t level) const { return std::max(mHeight >> level, 1u); }
        std::uint32_t getDepthAt(std::uint32_t level) const { return std::max(mDepth >> level, 1u); }

    private:
        /// The same barrier `transition` records, over `count` levels from `base`.
        void transitionLevels(VkCommandBuffer commands, std::uint32_t base, std::uint32_t count, VkImageLayout from,
            VkImageLayout to, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
            VkAccessFlags2 dstAccess) const;

        const Device& mDevice;
        VkImage mHandle = VK_NULL_HANDLE;
        VkImageView mView = VK_NULL_HANDLE;
        VkImageView mStorageView = VK_NULL_HANDLE;
        DeviceMemory mMemory;
        std::uint32_t mWidth = 0;
        std::uint32_t mHeight = 0;
        std::uint32_t mDepth = 1;
        VkFormat mFormat = VK_FORMAT_UNDEFINED;
        VkImageUsageFlags mUsage = 0;
        std::uint32_t mMipLevels = 1;
        std::uint32_t mTexelBytes = 0;
    };
}
