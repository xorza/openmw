#pragma once

#include <span>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace Rtx
{
    /// Which present modes of one surface the driver paces under, as `VkLatencySurfaceCapabilitiesNV`
    /// lists them: the surface's half of whether frames are paced, the device's being
    /// `Device::hasLatencyPacing`. Read once per surface, because the answer is the surface's and
    /// not the swapchain's.
    class PacedModes
    {
    public:
        /// Nothing paced: a headless renderer, a loader without the question, a driver without
        /// the answer.
        PacedModes() = default;

        /// Asks the surface. `ask` is `Instance::getSurfaceCapabilities2`, and null answers nothing
        /// paced.
        PacedModes(PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR ask, VkPhysicalDevice device, VkSurfaceKHR surface);

        bool paces(VkPresentModeKHR mode) const;
        std::span<const VkPresentModeKHR> get() const { return mModes; }

    private:
        std::vector<VkPresentModeKHR> mModes;
    };
}
