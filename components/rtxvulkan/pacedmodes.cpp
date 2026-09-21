#include "pacedmodes.hpp"

#include <algorithm>
#include <cstdint>

#include "result.hpp"

namespace Rtx
{
    PacedModes::PacedModes(const PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR ask, const VkPhysicalDevice device,
        const VkSurfaceKHR surface)
    {
        if (ask == nullptr)
            return;

        // The count and the list live in the chained structure rather than in the call's own
        // arguments, because this is the surface's capabilities asked a question the base
        // structure has no room for; the two-call shape is the same, so the enumeration is.
        const VkPhysicalDeviceSurfaceInfo2KHR asked{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR,
            .surface = surface,
        };
        VkLatencySurfaceCapabilitiesNV paced{ .sType = VK_STRUCTURE_TYPE_LATENCY_SURFACE_CAPABILITIES_NV };
        VkSurfaceCapabilities2KHR capabilities{ .sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR,
            .pNext = &paced };

        mModes = enumerateVk<VkPresentModeKHR>(
            "vkGetPhysicalDeviceSurfaceCapabilities2KHR", [&](std::uint32_t* count, VkPresentModeKHR* into) {
                paced.presentModeCount = *count;
                paced.pPresentModes = into;
                const VkResult result = ask(device, &asked, &capabilities);
                *count = paced.presentModeCount;
                return result;
            });
    }

    bool PacedModes::paces(const VkPresentModeKHR mode) const
    {
        return std::find(mModes.begin(), mModes.end(), mode) != mModes.end();
    }
}
