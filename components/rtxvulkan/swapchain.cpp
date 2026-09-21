#include "swapchain.hpp"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

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
            const std::vector<VkSurfaceFormatKHR> formats = enumerateVk<VkSurfaceFormatKHR>(
                "vkGetPhysicalDeviceSurfaceFormatsKHR", [&](std::uint32_t* count, VkSurfaceFormatKHR* into) {
                    return vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, count, into);
                });

            if (formats.empty())
                throw Unsupported("the surface offers no formats");

            // A plain unsigned-normalised format, because the blit that fills it converts between
            // formats and an sRGB target would encode an image that `TonePass` has already encoded:
            // the curve and the transfer function have both run by the time anything reaches here.
            for (const VkSurfaceFormatKHR& format : formats)
                if (format.format == VK_FORMAT_B8G8R8A8_UNORM || format.format == VK_FORMAT_R8G8B8A8_UNORM)
                    return format;

            Log(Debug::Warning) << "This surface offers no unsigned-normalised format, so the blit that "
                                   "fills it will encode an image that is already display-referred.";
            return formats.front();
        }

        /// What the surface will accept, in the order the caller would rather have. FIFO is the
        /// only mode a surface must support, so it ends every list here and nothing below has to
        /// answer for a driver that offers little else.
        VkPresentModeKHR chooseFrom(
            VkPhysicalDevice device, VkSurfaceKHR surface, std::initializer_list<VkPresentModeKHR> wanted)
        {
            const std::vector<VkPresentModeKHR> modes = enumerateVk<VkPresentModeKHR>(
                "vkGetPhysicalDeviceSurfacePresentModesKHR", [&](std::uint32_t* count, VkPresentModeKHR* into) {
                    return vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, count, into);
                });

            for (const VkPresentModeKHR mode : wanted)
                if (std::find(modes.begin(), modes.end(), mode) != modes.end())
                    return mode;

            return VK_PRESENT_MODE_FIFO_KHR;
        }

        /// The present mode a vertical sync setting asks for. `Disabled` is mailbox and not
        /// immediate, because what a player turning vsync off reaches for is latency and not a
        /// torn frame; immediate stands behind it for a surface with no mailbox — and ahead of it
        /// where the driver paces immediate and the player asked for the pacing, because the
        /// pacing is that latency, and the driver offers it under immediate and not under mailbox
        /// (measured on this driver: it paces immediate and relaxed FIFO, and neither of the other
        /// two). `Adaptive` is FIFO that tears when a frame misses its refresh, as the rasterizer's
        /// does through SDL. `Enabled` stays FIFO whether or not the driver paces it: a frame that
        /// meets the refresh is the promise, and relaxed FIFO breaks it to be paced.
        VkPresentModeKHR presentModeFor(VkPhysicalDevice device, VkSurfaceKHR surface, SDLUtil::VSyncMode mode,
            const bool preferPaced, const PacedModes& paced)
        {
            switch (mode)
            {
                case SDLUtil::VSyncMode::Disabled:
                    if (preferPaced && paced.paces(VK_PRESENT_MODE_IMMEDIATE_KHR))
                        return chooseFrom(
                            device, surface, { VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR });
                    return chooseFrom(device, surface, { VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR });
                case SDLUtil::VSyncMode::Adaptive:
                    return chooseFrom(device, surface, { VK_PRESENT_MODE_FIFO_RELAXED_KHR });
                case SDLUtil::VSyncMode::Enabled:
                    break;
            }

            return VK_PRESENT_MODE_FIFO_KHR;
        }

        std::string_view nameOf(VkPresentModeKHR mode)
        {
            switch (mode)
            {
                case VK_PRESENT_MODE_MAILBOX_KHR:
                    return "mailbox";
                case VK_PRESENT_MODE_IMMEDIATE_KHR:
                    return "immediate";
                case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
                    return "fifo relaxed";
                default:
                    return "fifo";
            }
        }
    }

    Swapchain::Swapchain(const Device& device, VkSurfaceKHR surface, VkExtent2D extent,
        const SDLUtil::VSyncMode verticalSync, const bool preferPaced, const PacedModes& paced)
        : mDevice(device)
        , mSurface(surface)
        , mVerticalSync(verticalSync)
        , mPacedModes(paced)
        , mPreferPaced(preferPaced)
    {
        VkBool32 supported = VK_FALSE;
        checkVk(vkGetPhysicalDeviceSurfaceSupportKHR(
                    device.getPhysicalDevice().getHandle(), device.getQueueFamily(), surface, &supported),
            "vkGetPhysicalDeviceSurfaceSupportKHR");
        if (supported != VK_TRUE)
            throw Unsupported("the queue this renderer submits on cannot present to this surface");

        mFormat = chooseFormat(device.getPhysicalDevice().getHandle(), surface);
        mPresentMode
            = presentModeFor(device.getPhysicalDevice().getHandle(), surface, mVerticalSync, mPreferPaced, mPacedModes);

        create(extent);

        // The surface's answer beside the mode in force, so a log says at a glance whether the
        // driver paces this window and which modes would have it.
        std::string pacedModes;
        for (const VkPresentModeKHR mode : mPacedModes.get())
            pacedModes += (pacedModes.empty() ? "" : ", ") + std::string(nameOf(mode));
        Log(Debug::Info) << "Swapchain: " << mImages.size() << " images, " << nameOf(mPresentMode)
                         << (mPaced ? ", paced by the driver" : "")
                         << (pacedModes.empty() ? std::string("; the driver paces no mode of this surface")
                                                : "; the driver paces " + pacedModes);
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

        // A minimised window reports no extent at all, and a swapchain of none is invalid usage.
        // One pixel rather than a refusal, because a window comes back: `Presenter::wantsResize`
        // declines to rebuild while the surface is hidden, and what stands until then costs a blit
        // of a single pixel.
        mExtent.width = std::max(mExtent.width, 1u);
        mExtent.height = std::max(mExtent.height, 1u);

        std::uint32_t images = capabilities.minImageCount + 1;
        if (capabilities.maxImageCount > 0)
            images = std::min(images, capabilities.maxImageCount);

        // What the surface will take, asked rather than assumed. The frame reaches the screen
        // as a blit, so a surface that will not be a transfer destination cannot be presented to at
        // all — and this renderer has no second way of filling one.
        if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0)
            throw Unsupported("this surface will not take a transfer, and the frame reaches it as a blit");

        // Opaque, and refused rather than substituted: the other modes blend a frame whose alpha
        // this renderer never set against whatever stands behind the window.
        if ((capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) == 0)
            throw Unsupported("this surface offers no opaque composite alpha, and the frame carries no alpha to blend");

        // Opted into the driver's pacing where the surface paces this mode, which is what makes
        // the sleep, the markers and the ids mean anything on this swapchain.
        mPaced = mDevice.hasLatencyPacing() && mPacedModes.paces(mPresentMode);
        const VkSwapchainLatencyCreateInfoNV paced{
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV,
            .latencyModeEnable = VK_TRUE,
        };

        const VkSwapchainCreateInfoKHR create{
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
            .pNext = mPaced ? &paced : nullptr,
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
        mHandle = Owned<VkSwapchainKHR, vkDestroySwapchainKHR>::make(
            mDevice.getHandle(), vkCreateSwapchainKHR, create, "vkCreateSwapchainKHR");

        mImages = enumerateVk<VkImage>("vkGetSwapchainImagesKHR", [&](std::uint32_t* count, VkImage* into) {
            return vkGetSwapchainImagesKHR(mDevice.getHandle(), mHandle.get(), count, into);
        });
    }

    void Swapchain::destroy()
    {
        mHandle.reset();
        mImages.clear();
    }

    void Swapchain::recreate(VkExtent2D extent)
    {
        destroy();
        create(extent);
    }

    bool Swapchain::setVerticalSync(SDLUtil::VSyncMode mode)
    {
        if (mode == mVerticalSync)
            return false;

        mVerticalSync = mode;
        return chooseMode();
    }

    bool Swapchain::setPreferPaced(const bool preferPaced)
    {
        if (preferPaced == mPreferPaced)
            return false;

        mPreferPaced = preferPaced;
        return chooseMode();
    }

    bool Swapchain::chooseMode()
    {
        // What the surface offers decides, so two settings can mean one mode. A driver with no
        // relaxed FIFO answers `Adaptive` with plain FIFO, and rebuilding the swapchain to arrive at
        // the mode it already had is a stall for nothing.
        const VkPresentModeKHR wanted = presentModeFor(
            mDevice.getPhysicalDevice().getHandle(), mSurface, mVerticalSync, mPreferPaced, mPacedModes);
        if (wanted == mPresentMode)
            return false;

        mPresentMode = wanted;
        Log(Debug::Info) << "Swapchain: presenting " << nameOf(mPresentMode);

        return true;
    }

    bool Swapchain::acquire(VkSemaphore ready, std::uint32_t& index)
    {
        // Bounded for the reason `awaitVk` is, and this is the wait a window is most likely to
        // sit in: a compositor that stops handing images back is indistinguishable from one that is
        // merely slow, and forever is not an answer a frame loop can act on.
        const VkResult result
            = vkAcquireNextImageKHR(mDevice.getHandle(), mHandle.get(), sPatience, ready, VK_NULL_HANDLE, &index);

        if (result == VK_ERROR_OUT_OF_DATE_KHR)
            return false;

        // Suboptimal still produces a usable image; taking it and rebuilding after the present keeps
        // the semaphore that was just signalled from being left dangling.
        if (result != VK_SUBOPTIMAL_KHR)
            checkVkWait(mDevice, result, "the presentation engine's next image", sPatience);

        return true;
    }

    bool Swapchain::present(VkSemaphore finished, std::uint32_t index, VkFence presented, const std::uint64_t presentId)
    {
        // The id ahead of the fence in the chain, each present only where it has one: the fence
        // where the device signals one, the id where the driver paces.
        const VkSwapchainPresentFenceInfoKHR signalled{
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR,
            .swapchainCount = 1,
            .pFences = &presented,
        };
        const VkPresentIdKHR identified{
            .sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR,
            .pNext = presented != VK_NULL_HANDLE ? &signalled : nullptr,
            .swapchainCount = 1,
            .pPresentIds = &presentId,
        };

        const VkSwapchainKHR presenting = mHandle.get();
        const VkPresentInfoKHR present{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext = presentId != 0           ? static_cast<const void*>(&identified)
                : presented != VK_NULL_HANDLE ? static_cast<const void*>(&signalled)
                                              : nullptr,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &finished,
            .swapchainCount = 1,
            .pSwapchains = &presenting,
            .pImageIndices = &index,
        };

        const VkResult result = vkQueuePresentKHR(mDevice.getQueue(), &present);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
            return false;

        checkVk(mDevice, result, "vkQueuePresentKHR");
        return true;
    }

    bool Swapchain::surfaceIsHidden() const
    {
        VkSurfaceCapabilitiesKHR capabilities{};
        checkVk(
            vkGetPhysicalDeviceSurfaceCapabilitiesKHR(mDevice.getPhysicalDevice().getHandle(), mSurface, &capabilities),
            "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

        return capabilities.currentExtent.width == 0 || capabilities.currentExtent.height == 0;
    }
}
