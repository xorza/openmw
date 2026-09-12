#include "deviceprofile.hpp"

#include <algorithm>
#include <optional>
#include <string_view>
#include <vector>

#include "memory.hpp"

namespace Rtx
{
    namespace
    {
        bool has(std::span<const std::string> names, std::string_view name)
        {
            return std::find(names.begin(), names.end(), name) != names.end();
        }

        std::string listMissingExtensions(std::span<const std::string> offered)
        {
            std::string missing;
            for (const char* const required : getRequiredDeviceExtensions())
                if (!has(offered, required))
                {
                    if (!missing.empty())
                        missing += ", ";
                    missing += required;
                }

            return missing;
        }

        std::string listMissingFeatures(DeviceFeatures& supported)
        {
            std::vector<std::string_view> lacking;
            findMissingFeatures(supported, lacking);

            std::string missing;
            for (const std::string_view feature : lacking)
            {
                if (!missing.empty())
                    missing += ", ";
                missing += feature;
            }

            return missing;
        }

        /// The largest heap holding a memory type a `Buffer::hostWritten` could come out of — the
        /// largest and not the sum, because a buffer goes in one heap.
        VkDeviceSize hostWrittenBytes(const VkPhysicalDeviceMemoryProperties& memory)
        {
            VkDeviceSize most = 0;
            for (std::uint32_t type = 0; type < memory.memoryTypeCount; ++type)
                if ((memory.memoryTypes[type].propertyFlags & sHostWritten) == sHostWritten)
                    most = std::max(most, memory.memoryHeaps[memory.memoryTypes[type].heapIndex].size);

            return most;
        }

        /// The queue family that can do everything this renderer submits, where there is one.
        std::optional<std::uint32_t> findQueueFamily(std::span<const VkQueueFamilyProperties> queues)
        {
            constexpr VkQueueFlags wanted = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
            for (std::uint32_t at = 0; at < queues.size(); ++at)
                if ((queues[at].queueFlags & wanted) == wanted)
                    return at;

            return std::nullopt;
        }
    }

    DeviceProfile profileOf(const DeviceProperties& properties, DeviceFeatures& supported,
        std::span<const std::string> extensions, std::span<const VkQueueFamilyProperties> queues)
    {
        DeviceProfile profile;
        profile.mHostWrittenBytes = hostWrittenBytes(properties.mMemory);

        const std::optional<std::uint32_t> family = findQueueFamily(queues);
        if (family.has_value())
        {
            profile.mQueueFamily = *family;
            profile.mTimestampBits = queues[*family].timestampValidBits;
        }

        for (const char* const name : getOptionalDeviceExtensions())
            if (has(extensions, name))
                profile.mOptionalExtensions.push_back(name);

        // **In the order a reader would want to be told.** A device short of the version cannot be
        // asked the rest of these questions meaningfully, and naming one missing feature of a card
        // that reports Vulkan 1.2 sends the reader after the wrong thing.
        if (properties.mProperties2.properties.apiVersion < sApiVersion)
        {
            profile.mObstacle = "reports Vulkan " + versionString(properties.mProperties2.properties.apiVersion);
            return profile;
        }

        if (const std::string missing = listMissingExtensions(extensions); !missing.empty())
        {
            profile.mObstacle = "missing extensions: " + missing;
            return profile;
        }

        if (const std::string missing = listMissingFeatures(supported); !missing.empty())
        {
            profile.mObstacle = "missing features: " + missing;
            return profile;
        }

        if (!family.has_value())
        {
            profile.mObstacle = "no queue family with graphics, compute and transfer";
            return profile;
        }

        // Nothing here stages, so a device the host cannot write into cannot run this at all. How
        // much room there is behind the type is answered rather than refused.
        if (profile.mHostWrittenBytes == 0)
        {
            profile.mObstacle = "no memory type the host writes into and the device reads";
            return profile;
        }

        return profile;
    }
}
