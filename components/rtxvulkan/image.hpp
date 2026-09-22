#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string_view>
#include <vector>

#include <vulkan/vulkan_core.h>

#include <components/rtx/result.hpp>

#include "imageuse.hpp"
#include "memory.hpp"
#include "owned.hpp"
#include "readstamp.hpp"

namespace Rtx
{
    class Buffer;
    class Device;

    /// An image, its allocation and its view.
    class Image
    {
    public:
        /// A slot with nothing in it yet: what a holder that has no extent until later keeps, and
        /// what a moved-from image is left as.
        Image() = default;

        /// @param name what a capture and a validation message call this image and its view.
        ///        Required, so a report naming one says which it was.
        /// @param mipLevels how many halvings the image holds, including the full one. Levels
        ///        past the first hold nothing until `buildMips` fills them, and one is an image
        ///        with no chain at all.
        /// @param depth how many slices it holds. One is a 2D image, which is what everything a
        ///        camera writes or a screen reads is; more makes it a volume, and a chain over one
        ///        halves the third axis with the other two.
        /// @param storageFormat the format a storage descriptor sees the image in, where that is
        ///        not `format`: an `SRGB` image has no storage view of its own, so a dispatch that
        ///        writes one stores display-encoded bytes through a `UNORM` view of the same bits,
        ///        and the sampler decodes them through the other. `VK_FORMAT_UNDEFINED` is
        ///        `format` itself, which is every image but such a one.
        Image(const Device& device, std::uint32_t width, std::uint32_t height, VkFormat format, VkImageUsageFlags usage,
            std::string_view name, std::uint32_t mipLevels = 1, std::uint32_t depth = 1,
            VkFormat storageFormat = VK_FORMAT_UNDEFINED);

        /// The same, for an image something stands in for: why there is none where the device has
        /// no room for it as `use` — `MemoryAllocator::tryTake`. The use first, so the parameters
        /// the constructor defaults stay last.
        static Result<Image, std::string_view> tryMake(MemoryUse use, const Device& device, std::uint32_t width,
            std::uint32_t height, VkFormat format, VkImageUsageFlags usage, std::string_view name,
            std::uint32_t mipLevels = 1, std::uint32_t depth = 1, VkFormat storageFormat = VK_FORMAT_UNDEFINED);

        /// Asserts that no submit still reads the image — `isIdle` — as a buffer's does: an image
        /// a submit may still read is buried, never destroyed.
        ~Image();

        /// Movable, because the channels of a g-buffer are built by a loop over a table rather
        /// than by a member list. `Owned` is what makes the move defaultable; the assignment is
        /// written out for the assert the destructor makes.
        Image(Image&&) noexcept = default;
        Image& operator=(Image&& other) noexcept;

        VkImage getHandle() const { return mHandle.get(); }

        bool isEmpty() const { return mHandle.get() == VK_NULL_HANDLE; }

        /// The view a sampler reads, which covers every level. A hand-out: names the image for
        /// the next submit, as every way a submit can reach it does.
        VkImageView getView() const
        {
            nameForNext();
            return mView.get();
        }

        /// The view a storage descriptor takes for `level`: one level alone, because Vulkan will
        /// not let a storage image name a chain. An image without a chain hands back the only view
        /// it has, and `level` must be one it holds. A hand-out, as `getView` is.
        VkImageView getStorageView(std::uint32_t level = 0) const
        {
            nameForNext();
            assert(level < mMipLevels);
            return mLevelViews.empty() ? mView.get() : mLevelViews[level].get();
        }

        /// Whether every submit that names this image has run — what the destructor asserts, and
        /// what the graveyard asserts as it frees. `ReadStamp::isIdle` says what a stamp for a
        /// submit not yet made means.
        bool isIdle() const;

        /// Blocks until `isIdle`, where a submit naming this image is still on the queue. `what`
        /// names the wait in the error a device that stops answering produces.
        void waitIdle(const char* what) const;

        /// This image as a storage descriptor takes it: `level`'s storage view, in `GENERAL`.
        /// Every storage image this renderer binds rests in `GENERAL`, and the view is the one a
        /// chain may not hand a storage descriptor — so a caller cannot pick the wrong one.
        VkDescriptorImageInfo describeStorage(std::uint32_t level = 0) const
        {
            return VkDescriptorImageInfo{ VK_NULL_HANDLE, getStorageView(level), VK_IMAGE_LAYOUT_GENERAL };
        }

        /// This image as a sampled descriptor takes it, through `sampler`, in `layout` — `GENERAL`
        /// for what a pass wrote as storage a few dispatches ago, and the read-only layout for a
        /// texture that rests there.
        VkDescriptorImageInfo describeSampled(VkSampler sampler, VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL) const
        {
            return VkDescriptorImageInfo{ sampler, getView(), layout };
        }

        std::uint32_t getWidth() const { return mWidth; }
        std::uint32_t getHeight() const { return mHeight; }
        VkFormat getFormat() const { return mFormat; }

        /// What this image was created able to do, kept so a mismatch can be asserted: an image
        /// sampled without `VK_IMAGE_USAGE_SAMPLED_BIT` reads as zero with no validation message.
        VkImageUsageFlags getUsage() const { return mUsage; }

        /// The same dependency as `transition`, for a caller collecting a run of them into one
        /// command. Every level, as `transition` is.
        VkImageMemoryBarrier2 describeTransition(const ImageUse& from, const ImageUse& to) const;

        /// Moves every level of the image from one use to the next, recording into `commands`.
        void transition(VkCommandBuffer commands, const ImageUse& from, const ImageUse& to) const;

        /// Clears every level to `colour`, met as `from` and left as `to`. Needs `TRANSFER_DST`.
        void clear(
            VkCommandBuffer commands, const ImageUse& from, const VkClearColorValue& colour, const ImageUse& to) const;

        /// Copies `extent` texels from this image's corner into `into`'s, this one in
        /// `TRANSFER_SRC_OPTIMAL` and `into` in `intoLayout`, which is the caller's to arrange
        /// either side.
        void copyTo(VkCommandBuffer commands, const Image& into, VkImageLayout intoLayout, VkExtent2D extent) const;

        /// Fills every level below the first by halving the one above it, in `VK_FILTER_LINEAR` —
        /// a box filter, which is what a moment wants: a channel carrying a square averages to a
        /// mean square. Takes and leaves the image in `VK_IMAGE_LAYOUT_GENERAL`, ordered against a
        /// sampled read. Needs both transfer usage bits.
        void buildMips(VkCommandBuffer commands) const;

        /// Copies one level to host memory, one texel's bytes per pixel, tightly packed, row by row.
        /// Left in the layout it was handed. Submits and waits, so it belongs to a screenshot
        /// rather than to a frame.
        void read(VkImageLayout layout, std::vector<std::uint8_t>& pixels, std::uint32_t level = 0) const;

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
        // Read by the tests and by nothing else.
        std::uint32_t getMipLevels() const { return mMipLevels; }

    private:
        /// The handle alone, with no memory bound and no view: what the constructor and `tryMake`
        /// both begin with, before either knows whether there is room.
        struct Unbound
        {
        };
        Image(Unbound, const Device& device, std::uint32_t width, std::uint32_t height, VkFormat format,
            VkImageUsageFlags usage, std::string_view name, std::uint32_t mipLevels, std::uint32_t depth,
            VkFormat storageFormat);

        /// Binds `memory` and makes the views, which is the rest of what the constructor does.
        void bind(DeviceMemory&& memory, std::string_view name);

        /// What the destructor and a move over this assert: empty, or nothing on the queue reads it.
        bool mayDestroy() const;

        /// Names this image for the next submit — every hand-out to a command or a descriptor
        /// funnels through here, which is what makes `isIdle` exact.
        void nameForNext() const;

        /// The same barrier `transition` records, over `count` levels from `base`.
        void transitionLevels(VkCommandBuffer commands, std::uint32_t base, std::uint32_t count, const ImageUse& from,
            const ImageUse& to) const;

        const Device* mDevice = nullptr;
        ReadStamp mRead;
        Owned<VkImage, vkDestroyImage> mHandle;
        Owned<VkImageView, vkDestroyImageView> mView;

        /// One view a level, for a chain something writes through as storage, or for a storage
        /// format that is not the image's; empty for an image that is neither, which is nearly
        /// every one.
        std::vector<Owned<VkImageView, vkDestroyImageView>> mLevelViews;
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

        /// The format a storage view is made in, where it is not `mFormat`, which `bind` reads.
        VkFormat mStorageFormat = VK_FORMAT_UNDEFINED;
    };

    /// A one-texel image for a binding a shader declares and a branch never reads, because a
    /// descriptor has to point somewhere and the real thing would be sixteen bytes a pixel of the
    /// frame. Laid out once by `usage` and never moved again. Submits and waits, so it belongs to a
    /// pass's construction rather than to a frame.
    Image makeStandIn(const Device& device, VkFormat format, VkImageUsageFlags usage, std::string_view name);
}
