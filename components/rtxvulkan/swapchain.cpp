#include "swapchain.hpp"

#include <algorithm>

#include <components/debug/debuglog.hpp>

#include <components/rtx/error.hpp>

#include "device.hpp"
#include "result.hpp"

namespace Rtx
{
    namespace
    {
        VkSurfaceFormatKHR chooseFormat(VkPhysicalDevice device, VkSurfaceKHR surface)
        {
            std::uint32_t count = 0;
            checkVk(vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &count, nullptr),
                "vkGetPhysicalDeviceSurfaceFormatsKHR");
            std::vector<VkSurfaceFormatKHR> formats(count);
            checkVk(vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &count, formats.data()),
                "vkGetPhysicalDeviceSurfaceFormatsKHR");

            if (formats.empty())
                throw Error("the surface offers no formats");

            // A plain unsigned-normalised format, because the blit that fills it converts between
            // formats and an sRGB target would silently encode an image that is not linear yet.
            // Tone mapping and the transfer function are M8's, and this is where they will land.
            for (const VkSurfaceFormatKHR& format : formats)
                if (format.format == VK_FORMAT_B8G8R8A8_UNORM || format.format == VK_FORMAT_R8G8B8A8_UNORM)
                    return format;

            Log(Debug::Warning) << "This surface offers no unsigned-normalised format, so the blit that "
                                   "fills it will encode an image that is already display-referred.";
            return formats.front();
        }

        VkPresentModeKHR choosePresentMode(VkPhysicalDevice device, VkSurfaceKHR surface)
        {
            std::uint32_t count = 0;
            checkVk(vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &count, nullptr),
                "vkGetPhysicalDeviceSurfacePresentModesKHR");
            std::vector<VkPresentModeKHR> modes(count);
            checkVk(vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &count, modes.data()),
                "vkGetPhysicalDeviceSurfacePresentModesKHR");

            // Mailbox for a tool someone is steering: it keeps the latest frame instead of queueing
            // it, so the view follows the mouse. FIFO is the only mode a surface must support.
            if (std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_MAILBOX_KHR) != modes.end())
                return VK_PRESENT_MODE_MAILBOX_KHR;

            return VK_PRESENT_MODE_FIFO_KHR;
        }
    }

    Swapchain::Swapchain(const Device& device, VkSurfaceKHR surface, VkExtent2D extent)
        : mDevice(device)
        , mSurface(surface)
    {
        VkBool32 supported = VK_FALSE;
        checkVk(vkGetPhysicalDeviceSurfaceSupportKHR(
                    device.getPhysicalDevice().getHandle(), device.getQueueFamily(), surface, &supported),
            "vkGetPhysicalDeviceSurfaceSupportKHR");
        if (supported != VK_TRUE)
            throw Error("the queue this renderer submits on cannot present to this surface");

        mFormat = chooseFormat(device.getPhysicalDevice().getHandle(), surface);
        mPresentMode = choosePresentMode(device.getPhysicalDevice().getHandle(), surface);

        create(extent);

        Log(Debug::Info) << "Swapchain: " << mImages.size() << " images, "
                         << (mPresentMode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "fifo");
    }

    Swapchain::~Swapchain()
    {
        destroy();
    }

    void Swapchain::create(VkExtent2D extent)
    {
        VkSurfaceCapabilitiesKHR capabilities{};
        checkVk(
            vkGetPhysicalDeviceSurfaceCapabilitiesKHR(mDevice.getPhysicalDevice().getHandle(), mSurface, &capabilities),
            "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

        // A compositor that has already decided the size says so here; otherwise the window's size
        // is the request, clamped to what the surface will accept.
        if (capabilities.currentExtent.width != UINT32_MAX)
            mExtent = capabilities.currentExtent;
        else
            mExtent = VkExtent2D{
                std::clamp(extent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width),
                std::clamp(extent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height),
            };

        std::uint32_t images = capabilities.minImageCount + 1;
        if (capabilities.maxImageCount > 0)
            images = std::min(images, capabilities.maxImageCount);

        const VkSwapchainCreateInfoKHR create{
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
            .surface = mSurface,
            .minImageCount = images,
            .imageFormat = mFormat.format,
            .imageColorSpace = mFormat.colorSpace,
            .imageExtent = mExtent,
            .imageArrayLayers = 1,
            .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .preTransform = capabilities.currentTransform,
            .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            .presentMode = mPresentMode,
            .clipped = VK_TRUE,
        };
        checkVk(vkCreateSwapchainKHR(mDevice.getHandle(), &create, nullptr, &mHandle), "vkCreateSwapchainKHR");

        std::uint32_t count = 0;
        checkVk(vkGetSwapchainImagesKHR(mDevice.getHandle(), mHandle, &count, nullptr), "vkGetSwapchainImagesKHR");
        mImages.resize(count);
        checkVk(
            vkGetSwapchainImagesKHR(mDevice.getHandle(), mHandle, &count, mImages.data()), "vkGetSwapchainImagesKHR");
    }

    void Swapchain::destroy()
    {
        if (mHandle != VK_NULL_HANDLE)
            vkDestroySwapchainKHR(mDevice.getHandle(), mHandle, nullptr);
        mHandle = VK_NULL_HANDLE;
        mImages.clear();
    }

    void Swapchain::recreate(VkExtent2D extent)
    {
        destroy();
        create(extent);
    }

    bool Swapchain::acquire(VkSemaphore ready, std::uint32_t& index)
    {
        // **Bounded for the reason `awaitVk` is**, and this is the wait a window is most likely to
        // sit in: a compositor that stops handing images back is indistinguishable from one that is
        // merely slow, and forever is not an answer a frame loop can act on.
        const VkResult result
            = vkAcquireNextImageKHR(mDevice.getHandle(), mHandle, sPatience, ready, VK_NULL_HANDLE, &index);

        if (result == VK_ERROR_OUT_OF_DATE_KHR)
            return false;

        if (result == VK_TIMEOUT)
            throw Error(timedOut("the presentation engine's next image", sPatience));

        // Suboptimal still produces a usable image; taking it and rebuilding after the present keeps
        // the semaphore that was just signalled from being left dangling.
        if (result != VK_SUBOPTIMAL_KHR)
            checkVk(result, "vkAcquireNextImageKHR");

        return true;
    }

    bool Swapchain::present(VkSemaphore finished, std::uint32_t index)
    {
        const VkPresentInfoKHR present{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &finished,
            .swapchainCount = 1,
            .pSwapchains = &mHandle,
            .pImageIndices = &index,
        };

        const VkResult result = vkQueuePresentKHR(mDevice.getQueue(), &present);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
            return false;

        checkVk(result, "vkQueuePresentKHR");
        return true;
    }
}
