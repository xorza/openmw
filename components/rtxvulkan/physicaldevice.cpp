#include "physicaldevice.hpp"

#include <cstring>

#include <algorithm>
#include <sstream>
#include <string_view>

#include <components/rtx/error.hpp>

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

        bool has(const std::vector<std::string>& names, std::string_view name)
        {
            return std::find(names.begin(), names.end(), name) != names.end();
        }

        /// The queue family that can do everything this renderer submits.
        ///
        /// Returns `-1` when there is none, which disqualifies the device.
        int findQueueFamily(VkPhysicalDevice device)
        {
            std::uint32_t count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
            std::vector<VkQueueFamilyProperties> families(count);
            vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());

            constexpr VkQueueFlags wanted = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
            for (std::uint32_t i = 0; i < count; ++i)
                if ((families[i].queueFlags & wanted) == wanted)
                    return static_cast<int>(i);

            return -1;
        }

        VkDeviceSize sumDeviceLocalHeaps(VkPhysicalDevice device)
        {
            VkPhysicalDeviceMemoryProperties memory{};
            vkGetPhysicalDeviceMemoryProperties(device, &memory);

            VkDeviceSize total = 0;
            for (std::uint32_t i = 0; i < memory.memoryHeapCount; ++i)
                if (memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                    total += memory.memoryHeaps[i].size;
            return total;
        }

        /// Why a candidate was rejected, or empty when it was not.
        ///
        /// Whether any memory type is video memory the host can write into directly.
        ///
        /// **Required, and not fallen back from.** Every table the frame rewrites — the instance
        /// rows, the light grid, the deformed vertices — is written straight into the memory the
        /// shader reads, which is what removes a staging buffer, a queue submit and a wait on the
        /// whole queue from each of them. Without it the renderer would need the staging path back,
        /// and a second way of doing this is a second thing to keep correct for hardware this fork
        /// does not target.
        bool hasResizableBar(VkPhysicalDevice handle)
        {
            constexpr VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
                | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

            VkPhysicalDeviceMemoryProperties memory{};
            vkGetPhysicalDeviceMemoryProperties(handle, &memory);

            for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i)
                if ((memory.memoryTypes[i].propertyFlags & wanted) == wanted)
                    return true;

            return false;
        }

        /// Fills `available` with the device's extensions on the way through: the caller needs the
        /// same list to work out which optional ones to enable, and enumerating twice for the device
        /// that qualifies is the sort of waste that spreads.
        std::string disqualify(
            VkPhysicalDevice handle, const DeviceProperties& properties, std::vector<std::string>& available)
        {
            available = getDeviceExtensions(handle);

            if (properties.mProperties2.properties.apiVersion < sApiVersion)
                return "reports Vulkan " + versionString(properties.mProperties2.properties.apiVersion);

            std::string missing;
            for (const char* const required : getRequiredDeviceExtensions())
                if (!has(available, required))
                {
                    if (!missing.empty())
                        missing += ", ";
                    missing += required;
                }
            if (!missing.empty())
                return "missing extensions: " + missing;

            DeviceFeatures supported;
            vkGetPhysicalDeviceFeatures2(handle, &supported.mFeatures2);

            std::vector<std::string_view> missingFeatures;
            findMissingFeatures(supported, missingFeatures);
            if (!missingFeatures.empty())
            {
                std::string names;
                for (const std::string_view feature : missingFeatures)
                {
                    if (!names.empty())
                        names += ", ";
                    names += feature;
                }
                return "missing features: " + names;
            }

            if (findQueueFamily(handle) < 0)
                return "no queue family with graphics, compute and transfer";

            if (!hasResizableBar(handle))
                return "no video memory the host can write: resizable BAR is off in firmware, or the "
                       "driver does not expose it";

            return {};
        }
    }

    PhysicalDevice PhysicalDevice::select(VkInstance instance)
    {
        std::uint32_t count = 0;
        checkVk(vkEnumeratePhysicalDevices(instance, &count, nullptr), "vkEnumeratePhysicalDevices");
        if (count == 0)
            throw Error("no Vulkan device is installed");

        std::vector<VkPhysicalDevice> handles(count);
        checkVk(vkEnumeratePhysicalDevices(instance, &count, handles.data()), "vkEnumeratePhysicalDevices");

        std::string rejections;
        PhysicalDevice best;
        bool bestIsDiscrete = false;

        for (const VkPhysicalDevice handle : handles)
        {
            auto properties = std::make_unique<DeviceProperties>();
            vkGetPhysicalDeviceProperties2(handle, &properties->mProperties2);

            std::vector<std::string> available;
            const std::string reason = disqualify(handle, *properties, available);
            if (!reason.empty())
            {
                rejections += "\n  ";
                rejections += properties->mProperties2.properties.deviceName;
                rejections += ": ";
                rejections += reason;
                continue;
            }

            const bool discrete
                = properties->mProperties2.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
            if (best.mHandle != VK_NULL_HANDLE && (bestIsDiscrete || !discrete))
                continue;

            std::vector<const char*> optional;
            for (const char* const name : getOptionalDeviceExtensions())
                if (has(available, name))
                    optional.push_back(name);

            best.mHandle = handle;
            best.mProperties = std::move(properties);
            best.mQueueFamily = static_cast<std::uint32_t>(findQueueFamily(handle));
            best.mOptionalExtensions = std::move(optional);
            best.mDeviceLocalMemory = sumDeviceLocalHeaps(handle);
            bestIsDiscrete = discrete;
        }

        if (best.mHandle == VK_NULL_HANDLE)
            throw Error("no Vulkan device meets this renderer's requirements:" + rejections);

        return best;
    }

    std::array<std::uint8_t, VK_UUID_SIZE> PhysicalDevice::getUuid() const
    {
        std::array<std::uint8_t, VK_UUID_SIZE> uuid{};
        std::memcpy(uuid.data(), mProperties->mVulkan11.deviceUUID, uuid.size());
        return uuid;
    }

    std::string PhysicalDevice::describe() const
    {
        const VkPhysicalDeviceProperties& base = mProperties->mProperties2.properties;
        const VkPhysicalDeviceRayTracingPipelinePropertiesKHR& rt = mProperties->mRayTracingPipeline;
        const VkPhysicalDeviceAccelerationStructurePropertiesKHR& as = mProperties->mAccelerationStructure;

        std::ostringstream out;
        out << "device:            " << base.deviceName << '\n'
            << "driver:            " << mProperties->mVulkan12.driverName << ' ' << mProperties->mVulkan12.driverInfo
            << '\n'
            << "Vulkan:            " << versionString(base.apiVersion) << '\n'
            << "device-local heap: " << mDeviceLocalMemory / (1024 * 1024) << " MiB\n"
            << "queue family:      " << mQueueFamily << '\n'
            << "subgroup size:     " << mProperties->mVulkan11.subgroupSize << '\n';

        // **Whether a frame can say where its own device time went.** Every per-pass figure this
        // renderer reports is a pair of timestamps scaled by this, so a device that cannot write
        // them reports nothing rather than something wrong — and this is where that shows.
        std::uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(mHandle, &families, nullptr);

        std::vector<VkQueueFamilyProperties> queues(families);
        vkGetPhysicalDeviceQueueFamilyProperties(mHandle, &families, queues.data());

        const std::uint32_t bits = mQueueFamily < families ? queues[mQueueFamily].timestampValidBits : 0;
        out << "timestamps:        ";
        if (bits == 0)
            out << "not on this queue\n";
        else
            out << bits << " bits at " << base.limits.timestampPeriod << " ns a tick\n";

        out << "\nray tracing\n"
            << "  shader group handle size:     " << rt.shaderGroupHandleSize << " bytes\n"
            << "  shader group base alignment:  " << rt.shaderGroupBaseAlignment << " bytes\n"
            << "  max ray recursion depth:      " << rt.maxRayRecursionDepth << '\n'
            << "  max ray dispatch invocations: " << rt.maxRayDispatchInvocationCount << '\n'
            << "  max geometry count:           " << as.maxGeometryCount << '\n'
            << "  max instance count:           " << as.maxInstanceCount << '\n'
            << "  max primitive count:          " << as.maxPrimitiveCount << '\n'
            << "  micromap subdivision levels:  " << mProperties->mOpacityMicromap.maxOpacity2StateSubdivisionLevel
            << " (2-state), " << mProperties->mOpacityMicromap.maxOpacity4StateSubdivisionLevel << " (4-state)\n";

        // A driver may expose the extension and then decline to reorder, which would make every
        // measurement of SER meaningless. The hint is the only way to tell.
        out << "  invocation reorder:           "
            << (mProperties->mInvocationReorder.rayTracingInvocationReorderReorderingHint
                           == VK_RAY_TRACING_INVOCATION_REORDER_MODE_REORDER_EXT
                       ? "reorders"
                       : "accepts the call and does nothing")
            << '\n';

        out << "\noptional extensions present\n";
        if (mOptionalExtensions.empty())
            out << "  (none)\n";
        for (const char* const name : mOptionalExtensions)
            out << "  " << name << '\n';

        return out.str();
    }
}
