#include <gtest/gtest.h>

#include <vulkan/vulkan_core.h>

#include <components/rtxvulkan/pacedmodes.hpp>

namespace Rtx
{
    namespace
    {
        /// Two present modes the surface paces under, answered the two-call way.
        VKAPI_ATTR VkResult VKAPI_CALL surfaceCapabilities(
            VkPhysicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR*, VkSurfaceCapabilities2KHR* capabilities)
        {
            auto* paced = static_cast<VkLatencySurfaceCapabilitiesNV*>(capabilities->pNext);
            if (paced->pPresentModes != nullptr)
            {
                paced->pPresentModes[0] = VK_PRESENT_MODE_MAILBOX_KHR;
                paced->pPresentModes[1] = VK_PRESENT_MODE_FIFO_KHR;
            }
            paced->presentModeCount = 2;
            return VK_SUCCESS;
        }

        /// The surface's list is read the two-call way, and a mode is paced where it is on it.
        TEST(RtxPacedModesTest, theSurfaceNamesTheModesItPacesUnder)
        {
            const PacedModes none;
            EXPECT_FALSE(none.paces(VK_PRESENT_MODE_FIFO_KHR));

            const PacedModes unasked(nullptr, VK_NULL_HANDLE, VK_NULL_HANDLE);
            EXPECT_TRUE(unasked.get().empty());

            const PacedModes asked(surfaceCapabilities, VK_NULL_HANDLE, VK_NULL_HANDLE);
            EXPECT_EQ(asked.get().size(), 2u);
            EXPECT_TRUE(asked.paces(VK_PRESENT_MODE_MAILBOX_KHR));
            EXPECT_TRUE(asked.paces(VK_PRESENT_MODE_FIFO_KHR));
            EXPECT_FALSE(asked.paces(VK_PRESENT_MODE_IMMEDIATE_KHR));
        }
    }
}
