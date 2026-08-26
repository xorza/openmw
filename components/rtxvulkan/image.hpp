#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "memory.hpp"

namespace Rtx
{
    class CommandPool;
    class Device;

    /// Whether an image's memory can leave this device.
    enum class Sharing
    {
        /// Ordinary. The allocation is this device's and nothing else can see it.
        Private,

        /// Allocated so another API can import it, and importable from another one's export.
        ///
        /// **The in-game path hands its frame to OpenGL rather than to a swapchain**, because the
        /// character doll, the local map, the global map and video playback are all OSG
        /// render-to-texture users and a Vulkan window would mean reimplementing every one of them
        /// before the game was playable again.
        Exportable,
    };

    /// A 2D image, its allocation and its view.
    class Image
    {
    public:
        /// @param name what a capture and a validation message call this image and its view.
        ///        Required, and not because every image deserves prose: they all used to be called
        ///        "target", so a report naming one said nothing about which it was.
        Image(const Device& device, std::uint32_t width, std::uint32_t height, VkFormat format, VkImageUsageFlags usage,
            std::string_view name, Sharing sharing = Sharing::Private);

        /// The same image, backed by an allocation another device or API already made.
        ///
        /// @param memory a file descriptor from `exportMemory`, whose ownership this takes: Vulkan
        ///        closes it when the import succeeds, and this closes it when the import throws.
        /// @param bytes the size that export reported. **Not width times height times bytes** — a
        ///        driver pads, and importing at the arithmetic size fails.
        Image(const Device& device, std::uint32_t width, std::uint32_t height, VkFormat format, VkImageUsageFlags usage,
            std::string_view name, int memory, VkDeviceSize bytes);

        ~Image();

        Image(const Image&) = delete;
        Image& operator=(const Image&) = delete;

        VkImage getHandle() const { return mHandle; }
        VkImageView getView() const { return mView; }
        std::uint32_t getWidth() const { return mWidth; }
        std::uint32_t getHeight() const { return mHeight; }
        VkFormat getFormat() const { return mFormat; }

        /// What this image was created able to do, which is not always what a reader assumes.
        ///
        /// **Kept so a mismatch can be asserted rather than looked at.** An image handed to
        /// something that samples it, without `VK_IMAGE_USAGE_SAMPLED_BIT`, reads as zero — no
        /// error, no validation message, just a black frame with nothing pointing at the cause.
        VkImageUsageFlags getUsage() const { return mUsage; }

        /// How many bytes one texel of this image's format occupies.
        std::uint32_t getTexelBytes() const { return mTexelBytes; }

        /// Moves the whole image to `layout`, recording into `commands`.
        void transition(VkCommandBuffer commands, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 srcStage,
            VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) const;

        /// A file descriptor another API can import this image's memory through, and how large that
        /// allocation is. The caller owns the descriptor and must close it.
        ///
        /// Only where the image was made `Sharing::Exportable`, which is asserted.
        int exportMemory() const { return mMemory.exportFd(); }
        VkDeviceSize getMemoryBytes() const { return mMemory.getSize(); }

        /// Copies the image to host memory, `getTexelBytes()` per pixel, tightly packed, row by row.
        ///
        /// **Left in the layout it was handed**, because reading an image is not a change to it and
        /// a caller that had to know a read moved it is one that would forget.
        ///
        /// Submits and waits, so it belongs to a screenshot rather than to a frame.
        void read(CommandPool& pool, VkImageLayout layout, std::vector<std::uint8_t>& pixels) const;

    private:
        const Device& mDevice;
        VkImage mHandle = VK_NULL_HANDLE;
        VkImageView mView = VK_NULL_HANDLE;
        DeviceMemory mMemory;
        std::uint32_t mWidth = 0;
        std::uint32_t mHeight = 0;
        VkFormat mFormat = VK_FORMAT_UNDEFINED;
        VkImageUsageFlags mUsage = 0;
        std::uint32_t mTexelBytes = 0;
    };
}
