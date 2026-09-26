#include "instanceobstacle.hpp"

#include <cstdint>
#include <span>
#include <string>

#include <vulkan/vulkan_core.h>

#include <components/rtx/error.hpp>
#include <components/rtxvulkan/instance.hpp>
#include <components/rtxvulkan/requirements.hpp>
#include <components/rtxvulkan/validation.hpp>

namespace Rtx::Testing
{
    std::string findInstanceObstacle()
    {
        std::uint32_t version = 0;
        if (vkEnumerateInstanceVersion(&version) != VK_SUCCESS || version < sApiVersion)
            return "the Vulkan loader is absent or older than this renderer requires";

        try
        {
            const Instance probe{ ValidationOptions{}, std::span<const char* const>{} };
        }
        catch (const Unsupported& obstacle)
        {
            return std::string("no Vulkan driver is installed: ") + obstacle.what();
        }

        return {};
    }
}
