#pragma once

#include <cstdint>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace Rtx
{
    class Device;

    /// The images the window presents, and the two calls that hand them back and forth.
    ///
    /// The renderer never draws into these. It renders into an image of its own and blits, because
    /// the format a surface offers is not one a compute shader may store to, and because every pass
    /// after M2 wants a high-precision target that no display could show anyway.
    class Swapchain
    {
    public:
        Swapchain(const Device& device, VkSurfaceKHR surface, VkExtent2D extent);
        ~Swapchain();

        Swapchain(const Swapchain&) = delete;
        Swapchain& operator=(const Swapchain&) = delete;

        /// Takes the next image. False means the swapchain no longer matches the window and must be
        /// recreated — which a resize, a monitor change or a compositor restart all cause, and none
        /// of which is an error.
        bool acquire(VkSemaphore ready, std::uint32_t& index);

        /// Hands the image back. False means the same thing as it does for `acquire`.
        bool present(VkSemaphore finished, std::uint32_t index);

        /// Rebuilds at a new size. The caller must have waited for every frame still in flight.
        void recreate(VkExtent2D extent);

        VkExtent2D getExtent() const { return mExtent; }
        VkImage getImage(std::uint32_t index) const { return mImages[index]; }
        std::uint32_t getImageCount() const { return static_cast<std::uint32_t>(mImages.size()); }

    private:
        void create(VkExtent2D extent);
        void destroy();

        const Device& mDevice;
        VkSurfaceKHR mSurface = VK_NULL_HANDLE;
        VkSwapchainKHR mHandle = VK_NULL_HANDLE;
        VkSurfaceFormatKHR mFormat{};
        VkPresentModeKHR mPresentMode = VK_PRESENT_MODE_FIFO_KHR;
        VkExtent2D mExtent{};
        std::vector<VkImage> mImages;
    };
}
