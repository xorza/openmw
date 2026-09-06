#include "physicaldevice.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <utility>

#include <components/rtx/error.hpp>

#include "memory.hpp"
#include "result.hpp"

namespace Rtx
{
    namespace
    {
        std::vector<std::string> getDeviceExtensions(VkPhysicalDevice device)
        {
            std::uint32_t count = 0;
            checkVk(vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr),
                "vkEnumerateDeviceExtensionProperties");
            std::vector<VkExtensionProperties> properties(count);
            checkVk(vkEnumerateDeviceExtensionProperties(device, nullptr, &count, properties.data()),
                "vkEnumerateDeviceExtensionProperties");

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

        /// Everything a device says about itself, and what this renderer makes of it.
        struct Candidate
        {
            std::unique_ptr<DeviceProperties> mProperties;
            DeviceProfile mProfile;
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

            found.mProfile = profileOf(*found.mProperties, supported, getDeviceExtensions(handle), queues);

            return found;
        }
    }

    bool PhysicalDevice::hasOptionalExtension(const char* name) const
    {
        const std::vector<const char*>& offered = mProfile.mOptionalExtensions;

        return std::any_of(offered.begin(), offered.end(),
            [name](const char* const listed) { return std::strcmp(listed, name) == 0; });
    }

    PhysicalDevice PhysicalDevice::select(VkInstance instance)
    {
        std::uint32_t count = 0;
        checkVk(vkEnumeratePhysicalDevices(instance, &count, nullptr), "vkEnumeratePhysicalDevices");
        if (count == 0)
            throw Unsupported("no Vulkan device is installed");

        std::vector<VkPhysicalDevice> handles(count);
        checkVk(vkEnumeratePhysicalDevices(instance, &count, handles.data()), "vkEnumeratePhysicalDevices");

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
            // **The heap a table the frame rewrites has to fit in**, which on a card without
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
        const VkPhysicalDeviceOpacityMicromapPropertiesEXT& micromap = mProperties->mOpacityMicromap;

        out << "\nray tracing\n"
            << "  max geometry count:           " << as.maxGeometryCount << '\n'
            << "  max instance count:           " << as.maxInstanceCount << '\n'
            << "  max primitive count:          " << as.maxPrimitiveCount << '\n'
            << "  shader group handle:          " << pipeline.shaderGroupHandleSize << " bytes, aligned "
            << pipeline.shaderGroupHandleAlignment << ", based " << pipeline.shaderGroupBaseAlignment << '\n'
            << "  max ray dispatch:             " << pipeline.maxRayDispatchInvocationCount
            << '\n'
            // **Reported and not required.** Ada added the hardware; every earlier RTX card
            // exposes the extension and reorders nothing, so a run that asks for a sort on one of
            // those is refused by name and a run that does not is the same trace either way.
            << "  reordering hint:              " << (mProfile.mReorders ? "reorder" : "none")
            << '\n'
            // **What a hit object may record and never execute**, which is the whole of what Stage 1
            // asks of the shader table. The field arrived with the extension's revision 2, so a
            // driver at revision 1 leaves it as it found it — printed rather than asserted against
            // for that reason.
            << "  max record index:             " << reorder.maxShaderBindingTableRecordIndex << '\n'
            << "  micromap levels:              " << micromap.maxOpacity2StateSubdivisionLevel << " two-state, "
            << micromap.maxOpacity4StateSubdivisionLevel << " four-state\n";

        out << "\noptional extensions present\n";
        if (mProfile.mOptionalExtensions.empty())
            out << "  (none)\n";
        for (const char* const name : mProfile.mOptionalExtensions)
            out << "  " << name << '\n';

        return out.str();
    }
}
