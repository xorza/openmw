#include "physicaldevice.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

#include <components/rtx/error.hpp>

#include "memory.hpp"
#include "result.hpp"

namespace Rtx
{
    namespace
    {
        std::vector<std::string> getDeviceExtensions(VkPhysicalDevice device)
        {
            const std::vector<VkExtensionProperties> properties = enumerateVk<VkExtensionProperties>(
                "vkEnumerateDeviceExtensionProperties", [&](std::uint32_t* count, VkExtensionProperties* into) {
                    return vkEnumerateDeviceExtensionProperties(device, nullptr, count, into);
                });

            std::vector<std::string> names;
            names.reserve(properties.size());
            for (const VkExtensionProperties& extension : properties)
                names.emplace_back(extension.extensionName);
            return names;
        }

        VkDeviceSize sumDeviceLocalHeaps(const VkPhysicalDeviceMemoryProperties& memory)
        {
            VkDeviceSize total = 0;
            for (std::uint32_t i = 0; i < memory.memoryHeapCount; ++i)
                if (memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                    total += memory.memoryHeaps[i].size;
            return total;
        }

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

        /// Everything a device says about itself, and what this renderer makes of it.
        struct Candidate
        {
            std::unique_ptr<DeviceProperties> mProperties;
            PhysicalDevice::Profile mProfile;
        };

        Candidate examine(VkPhysicalDevice handle)
        {
            Candidate found;
            found.mProperties = std::make_unique<DeviceProperties>();
            vkGetPhysicalDeviceProperties2(handle, &found.mProperties->mProperties2);
            vkGetPhysicalDeviceMemoryProperties(handle, &found.mProperties->mMemory);

            DeviceFeatures supported;
            vkGetPhysicalDeviceFeatures2(handle, &supported.mFeatures2);

            std::uint32_t families = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(handle, &families, nullptr);
            std::vector<VkQueueFamilyProperties> queues(families);
            vkGetPhysicalDeviceQueueFamilyProperties(handle, &families, queues.data());

            found.mProfile
                = PhysicalDevice::profileOf(*found.mProperties, supported, getDeviceExtensions(handle), queues);

            return found;
        }
    }

    PhysicalDevice::Profile PhysicalDevice::profileOf(const DeviceProperties& properties, DeviceFeatures& supported,
        std::span<const std::string> extensions, std::span<const VkQueueFamilyProperties> queues)
    {
        Profile profile;
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

        // In the order a reader would want to be told. A device short of the version cannot be
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

    bool PhysicalDevice::hasOptionalExtension(const char* name) const
    {
        const std::vector<const char*>& offered = mProfile.mOptionalExtensions;

        return std::any_of(offered.begin(), offered.end(),
            [name](const char* const listed) { return std::strcmp(listed, name) == 0; });
    }

    PhysicalDevice PhysicalDevice::select(VkInstance instance)
    {
        const std::vector<VkPhysicalDevice> handles = enumerateVk<VkPhysicalDevice>(
            "vkEnumeratePhysicalDevices", [&](std::uint32_t* count, VkPhysicalDevice* into) {
                return vkEnumeratePhysicalDevices(instance, count, into);
            });
        if (handles.empty())
            throw Unsupported("no Vulkan device is installed");

        std::string rejections;
        PhysicalDevice best;
        bool bestIsDiscrete = false;

        for (const VkPhysicalDevice handle : handles)
        {
            Candidate found = examine(handle);
            if (!found.mProfile.mObstacle.empty())
            {
                rejections += "\n  ";
                rejections += found.mProperties->mProperties2.properties.deviceName;
                rejections += ": ";
                rejections += found.mProfile.mObstacle;
                continue;
            }

            const bool discrete
                = found.mProperties->mProperties2.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
            if (best.mHandle != VK_NULL_HANDLE && (bestIsDiscrete || !discrete))
                continue;

            best.mHandle = handle;
            best.mProperties = std::move(found.mProperties);
            best.mProfile = std::move(found.mProfile);
            bestIsDiscrete = discrete;
        }

        if (best.mHandle == VK_NULL_HANDLE)
            throw Unsupported("no Vulkan device meets this renderer's requirements:" + rejections);

        return best;
    }

    std::string PhysicalDevice::describe() const
    {
        const VkPhysicalDeviceProperties& base = mProperties->mProperties2.properties;
        const VkPhysicalDeviceAccelerationStructurePropertiesKHR& as = mProperties->mAccelerationStructure;

        std::ostringstream out;
        out << "device:            " << base.deviceName << '\n'
            << "driver:            " << mProperties->mVulkan12.driverName << ' ' << mProperties->mVulkan12.driverInfo
            << '\n'
            << "Vulkan:            " << versionString(base.apiVersion) << '\n'
            << "device-local heap: " << sumDeviceLocalHeaps(mProperties->mMemory) / (1024 * 1024)
            << " MiB\n"
            // The heap a table the frame rewrites has to fit in, which on a card without
            // resizable BAR is a couple of hundred megabytes of the line above rather than all of
            // it. Printed beside it because the two look alike and only one of them bounds a scene.
            << "host-written:      " << mProfile.mHostWrittenBytes / (1024 * 1024) << " MiB\n"
            << "queue family:      " << mProfile.mQueueFamily << '\n'
            << "subgroup size:     " << mProperties->mVulkan11.subgroupSize << '\n';

        out << "timestamps:        ";
        if (mProfile.mTimestampBits == 0)
            out << "not on this queue\n";
        else
            out << mProfile.mTimestampBits << " bits at " << base.limits.timestampPeriod << " ns a tick\n";

        const VkPhysicalDeviceRayTracingPipelinePropertiesKHR& pipeline = mProperties->mRayTracingPipeline;
        const VkPhysicalDeviceRayTracingInvocationReorderPropertiesEXT& reorder = mProperties->mInvocationReorder;

        out << "\nray tracing\n"
            << "  max geometry count:           " << as.maxGeometryCount << '\n'
            << "  max instance count:           " << as.maxInstanceCount << '\n'
            << "  max primitive count:          " << as.maxPrimitiveCount << '\n'
            << "  shader group handle:          " << pipeline.shaderGroupHandleSize << " bytes, aligned "
            << pipeline.shaderGroupHandleAlignment << ", based " << pipeline.shaderGroupBaseAlignment << '\n'
            << "  max ray dispatch:             " << pipeline.maxRayDispatchInvocationCount
            << '\n'
            // What a hit object may name. The field arrived with the extension's revision 2, so
            // a driver at revision 1 leaves it as it found it — printed rather than asserted against
            // for that reason.
            << "  max record index:             " << reorder.maxShaderBindingTableRecordIndex << '\n';

        out << "\noptional extensions present\n";
        if (mProfile.mOptionalExtensions.empty())
            out << "  (none)\n";
        for (const char* const name : mProfile.mOptionalExtensions)
            out << "  " << name << '\n';

        return out.str();
    }
}
