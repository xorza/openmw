#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "imageuse.hpp"
#include "memory.hpp"
#include "owned.hpp"

namespace Rtx
{
    class Buffer;
    class CommandPool;
    class Device;

    /// An image, its allocation and its view.
    class Image
    {
    public:
        /// @param name what a capture and a validation message call this image and its view.
        ///        Required, so a report naming one says which it was.
        /// @param mipLevels how many halvings the image holds, including the full one. Levels
        ///        past the first hold nothing until `buildMips` fills them, and one is an image
        ///        with no chain at all.
        /// @param depth how many slices it holds. One is a 2D image, which is what everything a
        ///        camera writes or a screen reads is; more makes it a volume, and a chain over one
        ///        halves the third axis with the other two.
        Image(const Device& device, std::uint32_t width, std::uint32_t height, VkFormat format, VkImageUsageFlags usage,
            std::string_view name, std::uint32_t mipLevels = 1, std::uint32_t depth = 1);

        /// **Movable, because the channels of a g-buffer are built by a loop over a table rather
        /// than by a member list.** `Owned` is what makes the moves defaultable.
        Image(Image&&) noexcept = default;
        Image& operator=(Image&&) noexcept = default;

        VkImage getHandle() const { return mHandle.get(); }

        /// The view a sampler reads, which covers every level.
        VkImageView getView() const { return mView.get(); }

        /// The view a storage descriptor takes, which is the first level alone.
        ///
        /// **Vulkan will not let a storage image name a chain**, so an image with one is written
        /// through a second view of its own — and an image without one hands back the only view it
        /// has, so nothing that never asked for levels has anything to know about this. An image
        /// with a chain and no storage usage has none either: nothing may name it there.
        VkImageView getStorageView() const
        {
            return mStorageView.get() != VK_NULL_HANDLE ? mStorageView.get() : mView.get();
        }
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

        /// The same dependency as `transition`, for a caller collecting a run of them into one
        /// command. Every level, as `transition` is.
        VkImageMemoryBarrier2 describeTransition(const ImageUse& from, const ImageUse& to) const;

        /// Moves every level of the image from one use to the next, recording into `commands`.
        void transition(VkCommandBuffer commands, const ImageUse& from, const ImageUse& to) const;

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

        /// Copies one level to host memory, one texel's bytes per pixel, tightly packed, row by row.
        ///
        /// **Left in the layout it was handed**, because reading an image is not a change to it and
        /// a caller that had to know a read moved it is one that would forget.
        ///
        /// Submits and waits, so it belongs to a screenshot rather than to a frame.
        void read(
            CommandPool& pool, VkImageLayout layout, std::vector<std::uint8_t>& pixels, std::uint32_t level = 0) const;

        /// What `read` records: one level copied into `into`, a host-readable buffer of at least
        /// `getReadBytes(level)`, ordered for the host, with the image met as `before` and left as
        /// `after`. For a copy that rides a batch rather than a wait of its own.
        void recordRead(VkCommandBuffer commands, const ImageUse& before, const ImageUse& after, const Buffer& into,
            std::uint32_t level = 0) const;

        /// How many bytes `read` and `recordRead` copy for `level`.
        VkDeviceSize getReadBytes(std::uint32_t level = 0) const;

        /// How wide, how tall and how deep `level` is, which is the full size halved that many
        /// times and never below one texel.
        std::uint32_t getWidthAt(std::uint32_t level) const { return std::max(mWidth >> level, 1u); }
        std::uint32_t getHeightAt(std::uint32_t level) const { return std::max(mHeight >> level, 1u); }
        std::uint32_t getDepthAt(std::uint32_t level) const { return std::max(mDepth >> level, 1u); }

    private:
        /// The same barrier `transition` records, over `count` levels from `base`.
        void transitionLevels(VkCommandBuffer commands, std::uint32_t base, std::uint32_t count, const ImageUse& from,
            const ImageUse& to) const;

        const Device* mDevice = nullptr;
        Owned<VkImage, vkDestroyImage> mHandle;
        Owned<VkImageView, vkDestroyImageView> mView;
        Owned<VkImageView, vkDestroyImageView> mStorageView;
        DeviceMemory mMemory;
        std::uint32_t mWidth = 0;
        std::uint32_t mHeight = 0;
        std::uint32_t mDepth = 1;
        VkFormat mFormat = VK_FORMAT_UNDEFINED;
        /// `describeTransition`'s own answer over a run of levels, which is what `transitionLevels`
        /// emits. One statement of the barrier, because the two differ only in the range.
        VkImageMemoryBarrier2 describeLevels(
            std::uint32_t base, std::uint32_t count, const ImageUse& from, const ImageUse& to) const;

        VkImageUsageFlags mUsage = 0;
        std::uint32_t mMipLevels = 1;
        std::uint32_t mTexelBytes = 0;
    };

    /// A one-texel image for a binding a shader declares and a branch never reads.
    ///
    /// **A descriptor has to point somewhere.** A caller that bound the real thing regardless would
    /// carry a full-size image for a binding nothing looks at — sixteen bytes a pixel of the frame.
    /// Laid out here once and then never moved again, because a bound image has to be in the
    /// layout its descriptor names whether the shader reads it or not.
    ///
    /// **What it is laid out for follows from `usage`**, because a storage image is read as one
    /// access and a sampled image as the other, and no caller has ever wanted the pair to disagree.
    ///
    /// Submits and waits, so it belongs to a pass's construction rather than to a frame.
    Image makeStandIn(
        const Device& device, CommandPool& pool, VkFormat format, VkImageUsageFlags usage, std::string_view name);

    /// A run of image dependencies emitted as one command.
    ///
    /// **One `vkCmdPipelineBarrier2` for a handover, not one per image.** The G-buffer's fourteen
    /// channels change state together twice a frame, and a `transition` apiece is twenty-eight
    /// commands where two would say the same thing — which is what NVIDIA's own guidance says to
    /// group. A batch that fills up emits what it holds and carries on, so a caller never has to
    /// know how many it is about to add.
    ///
    /// **No allocation, because this is a frame path.** The longest run in this renderer is the
    /// G-buffer's channels; the array is sized for that and flushing covers anything longer.
    class Barriers
    {
    public:
        explicit Barriers(VkCommandBuffer commands)
            : mCommands(commands)
        {
        }

        void add(const VkImageMemoryBarrier2& barrier);

        /// Emits what has been added, and empties. Does nothing where nothing was added.
        void flush();

    private:
        /// The longest run this renderer has: the G-buffer's channels, which change state together
        /// twice a frame. A run longer than this emits what it holds and carries on, so the figure
        /// bounds the array rather than the caller.
        static constexpr std::size_t sMost = 16;

        VkCommandBuffer mCommands = VK_NULL_HANDLE;
        std::array<VkImageMemoryBarrier2, sMost> mBarriers{};
        std::size_t mCount = 0;
    };
}
