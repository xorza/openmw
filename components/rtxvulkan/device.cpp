#include "device.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <components/debug/debuglog.hpp>
#include <components/rtx/error.hpp>

#include "dlss.hpp"
#include "instance.hpp"
#include "pipelinecache.hpp"
#include "result.hpp"

namespace Rtx
{
    namespace
    {
        /// What a faulting address was being used for, as the header spells it.
        std::string_view faultAddressTypeName(VkDeviceFaultAddressTypeEXT type)
        {
            switch (type)
            {
                case VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_EXT:
                    return "a fault at no address";
                case VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT:
                    return "an invalid read";
                case VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT:
                    return "an invalid write";
                case VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_EXT:
                    return "an invalid execute";
                case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_UNKNOWN_EXT:
                    return "an instruction pointer the driver could not place";
                case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_EXT:
                    return "an instruction pointer outside any shader";
                case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_EXT:
                    return "the instruction that faulted";
                default:
                    return "an address of unrecognised type";
            }
        }

        template <class T>
        void load(VkDevice device, T& out, const char* name)
        {
            out = reinterpret_cast<T>(vkGetDeviceProcAddr(device, name));
            if (out == nullptr)
                throw Error(
                    std::string("the driver advertises the extension providing ") + name + " but does not dispatch it");
        }
    }

    Device::Device(const Instance& instance, PhysicalDevice&& physicalDevice, const PipelineCacheSpec& cache,
        const std::vector<const char*>& extraExtensions)
        : mPhysicalDevice(std::move(physicalDevice))
    {
        std::vector<const char*> extensions;
        for (const char* const name : getRequiredDeviceExtensions())
            extensions.push_back(name);
        for (const char* const name : mPhysicalDevice.getAvailableOptionalExtensions())
            extensions.push_back(name);
#ifdef OPENMW_RTX_DLSS
        // What NGX asks for, which it will not start without. Appended rather than added to the
        // required list because that list is what this renderer needs to trace at all, and a build
        // without DLSS must not fail on a device that lacks them.
        for (const char* const name : Dlss::getDeviceExtensions())
        {
            // **`VK_EXT_buffer_device_address` cannot come along**, and not because it is missing:
            // the feature it provides is Vulkan 1.2 core here, enabled through
            // `VkPhysicalDeviceVulkan12Features`, and the spec forbids asking for both. NGX names
            // the pre-1.2 spelling because it supports drivers older than this one does.
            if (std::strcmp(name, VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME) == 0)
                continue;

            const auto already = [&](const char* const listed) { return std::strcmp(listed, name) == 0; };
            if (std::none_of(extensions.begin(), extensions.end(), already))
                extensions.push_back(name);
        }
#endif

        for (const char* const name : extraExtensions)
            extensions.push_back(name);

        DeviceFeatures features;
        requestRequiredFeatures(features);

        // **Outside `DeviceFeatures`, because it is the one feature that is optional.** That type is
        // what the renderer requires, asked and enabled as one list, and a feature it can do without
        // has no place in a list a device is refused for lacking. Asked of the physical device here,
        // because a driver may offer the extension without the feature, and chained ahead of the
        // required ones where it has it.
        VkPhysicalDeviceFaultFeaturesEXT fault{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT };
        if (mPhysicalDevice.hasOptionalExtension(VK_EXT_DEVICE_FAULT_EXTENSION_NAME))
        {
            VkPhysicalDeviceFeatures2 offered{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &fault };
            vkGetPhysicalDeviceFeatures2(mPhysicalDevice.getHandle(), &offered);

            // The vendor binary is not asked for: nothing here could read it, and a feature enabled
            // for nothing is a feature to explain.
            fault.deviceFaultVendorBinary = VK_FALSE;
            fault.pNext = &features.mFeatures2;
        }
        const bool describesFault = fault.deviceFault == VK_TRUE;

        const float priority = 1.0f;
        const VkDeviceQueueCreateInfo queue{
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = mPhysicalDevice.getQueueFamily(),
            .queueCount = 1,
            .pQueuePriorities = &priority,
        };

        const VkDeviceCreateInfo createInfo{
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .pNext = describesFault ? static_cast<const void*>(&fault) : &features.mFeatures2,
            .queueCreateInfoCount = 1,
            .pQueueCreateInfos = &queue,
            .enabledExtensionCount = static_cast<std::uint32_t>(extensions.size()),
            .ppEnabledExtensionNames = extensions.data(),
            // Superseded by the VkPhysicalDeviceFeatures2 in the chain, and the two cannot both be set.
            .pEnabledFeatures = nullptr,
        };

        checkVk(vkCreateDevice(mPhysicalDevice.getHandle(), &createInfo, nullptr, &mHandle), "vkCreateDevice");

        // A constructor that throws runs no destructor, and a driver that advertises an extension it
        // cannot dispatch is exactly the case this reports — so the device goes back before the
        // failure leaves here.
        try
        {
            vkGetDeviceQueue(mHandle, mPhysicalDevice.getQueueFamily(), 0, &mQueue);

            load(mHandle, mFunctions.mGetAccelerationStructureBuildSizes, "vkGetAccelerationStructureBuildSizesKHR");
            load(mHandle, mFunctions.mCreateAccelerationStructure, "vkCreateAccelerationStructureKHR");
            load(mHandle, mFunctions.mDestroyAccelerationStructure, "vkDestroyAccelerationStructureKHR");
            load(mHandle, mFunctions.mCmdBuildAccelerationStructures, "vkCmdBuildAccelerationStructuresKHR");
            load(mHandle, mFunctions.mGetAccelerationStructureDeviceAddress,
                "vkGetAccelerationStructureDeviceAddressKHR");
            load(mHandle, mFunctions.mCreateRayTracingPipelines, "vkCreateRayTracingPipelinesKHR");
            load(mHandle, mFunctions.mGetRayTracingShaderGroupHandles, "vkGetRayTracingShaderGroupHandlesKHR");
            load(mHandle, mFunctions.mCmdTraceRays, "vkCmdTraceRaysKHR");
            load(mHandle, mFunctions.mGetPipelineExecutableProperties, "vkGetPipelineExecutablePropertiesKHR");
            load(mHandle, mFunctions.mGetPipelineExecutableStatistics, "vkGetPipelineExecutableStatisticsKHR");
            load(mHandle, mFunctions.mGetMicromapBuildSizes, "vkGetMicromapBuildSizesEXT");
            load(mHandle, mFunctions.mCreateMicromap, "vkCreateMicromapEXT");
            load(mHandle, mFunctions.mDestroyMicromap, "vkDestroyMicromapEXT");
            load(mHandle, mFunctions.mCmdBuildMicromaps, "vkCmdBuildMicromapsEXT");

            if (describesFault)
                load(mHandle, mGetDeviceFaultInfo, "vkGetDeviceFaultInfoEXT");

            if (instance.hasDebugUtils())
            {
                mSetObjectName = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
                    vkGetDeviceProcAddr(mHandle, "vkSetDebugUtilsObjectNameEXT"));
                mBeginLabel = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
                    vkGetDeviceProcAddr(mHandle, "vkCmdBeginDebugUtilsLabelEXT"));
                mEndLabel = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
                    vkGetDeviceProcAddr(mHandle, "vkCmdEndDebugUtilsLabelEXT"));
            }

            mPipelineCache = std::make_unique<PipelineCache>(
                mHandle, mPhysicalDevice.getProperties().mProperties2.properties, cache);
        }
        catch (...)
        {
            vkDestroyDevice(mHandle, nullptr);
            mHandle = VK_NULL_HANDLE;
            throw;
        }
    }

    Device::~Device()
    {
        if (mHandle != VK_NULL_HANDLE)
        {
            vkDeviceWaitIdle(mHandle);

            // Before the device it was made on, and explicitly rather than by member order: saving
            // it calls into the device, so it cannot outlive one this destructor is about to close.
            mPipelineCache.reset();

            vkDestroyDevice(mHandle, nullptr);
        }
    }

    VkPipelineCache Device::getPipelineCache() const
    {
        return mPipelineCache->getHandle();
    }

    void Device::reportPipeline(VkPipeline pipeline, std::string_view name) const
    {
        const VkPipelineInfoKHR asked{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR,
            .pipeline = pipeline,
        };

        std::uint32_t executables = 0;
        checkVk(mFunctions.mGetPipelineExecutableProperties(mHandle, &asked, &executables, nullptr),
            "vkGetPipelineExecutablePropertiesKHR");

        // **Silence and "nothing to say" are different answers, and this is the second.** NVIDIA's
        // compiler reports no executable at all for a ray tracing pipeline, where it reports one for
        // every compute pipeline in this renderer — so the trace's register count, which is what an
        // occupancy figure is made of, is not a number this device will give. Said once per pipeline
        // rather than left as a missing line, because a reader otherwise cannot tell it from a call
        // nobody made.
        //
        // **And no internal representation for any pipeline**, asked with the capture flag set: the
        // driver answers with a count of nought, so which load a kernel compiled to is not a question
        // this extension can put to it. Nsight Graphics is where that is read.
        if (executables == 0)
        {
            Log(Debug::Verbose) << "pipeline " << name << ": the driver reports no executable";
            return;
        }

        for (std::uint32_t executable = 0; executable < executables; ++executable)
        {
            const VkPipelineExecutableInfoKHR which{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR,
                .pipeline = pipeline,
                .executableIndex = executable,
            };

            std::uint32_t count = 0;
            checkVk(mFunctions.mGetPipelineExecutableStatistics(mHandle, &which, &count, nullptr),
                "vkGetPipelineExecutableStatisticsKHR");

            std::vector<VkPipelineExecutableStatisticKHR> statistics(count);
            for (VkPipelineExecutableStatisticKHR& statistic : statistics)
                statistic.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR;
            checkVk(mFunctions.mGetPipelineExecutableStatistics(mHandle, &which, &count, statistics.data()),
                "vkGetPipelineExecutableStatisticsKHR");

            // **Whatever the driver chose to say, and not a list this side picked.** The names are
            // the compiler's own — NVIDIA reports registers and spills, another vendor reports
            // something else — so a fixed set of fields here would be a set that goes empty on the
            // next driver. `openmw-rtxtool` prints them verbatim.
            std::string line;
            for (const VkPipelineExecutableStatisticKHR& statistic : statistics)
            {
                if (!line.empty())
                    line += ", ";
                line += statistic.name;
                line += ' ';

                switch (statistic.format)
                {
                    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR:
                        line += statistic.value.b32 != VK_FALSE ? "yes" : "no";
                        break;
                    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR:
                        line += std::to_string(statistic.value.i64);
                        break;
                    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR:
                        line += std::to_string(statistic.value.u64);
                        break;
                    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR:
                        line += std::to_string(statistic.value.f64);
                        break;
                    default:
                        line += '?';
                        break;
                }
            }

            Log(Debug::Verbose) << "pipeline " << name << ": " << line;
        }
    }

    void Device::waitIdle() const
    {
        checkVk(*this, vkDeviceWaitIdle(mHandle), "vkDeviceWaitIdle");
    }

    std::string Device::describeFault() const
    {
        if (mGetDeviceFaultInfo == nullptr)
            return {};

        constexpr const char* sUnsaid = "\nthe driver would not say where the device faulted";

        VkDeviceFaultCountsEXT counts{ .sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT };
        if (mGetDeviceFaultInfo(mHandle, &counts, nullptr) != VK_SUCCESS)
            return sUnsaid;

        std::vector<VkDeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
        std::vector<VkDeviceFaultVendorInfoEXT> vendor(counts.vendorInfoCount);

        // Nought, because the feature that provides the binary was not enabled.
        counts.vendorBinarySize = 0;

        VkDeviceFaultInfoEXT info{
            .sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT,
            .pAddressInfos = addresses.data(),
            .pVendorInfos = vendor.data(),
        };

        // `VK_INCOMPLETE` is the driver having more to say than the counts it gave a moment ago
        // allowed for, and what it did say is still worth reading.
        const VkResult result = mGetDeviceFaultInfo(mHandle, &counts, &info);
        if (result != VK_SUCCESS && result != VK_INCOMPLETE)
            return sUnsaid;

        std::string report = "\ndevice fault: ";
        report += info.description;
        for (std::uint32_t at = 0; at < counts.addressInfoCount; ++at)
            report += std::format("\n  {} at {:#x}, known to within {:#x}",
                faultAddressTypeName(addresses[at].addressType), addresses[at].reportedAddress,
                addresses[at].addressPrecision);
        for (std::uint32_t at = 0; at < counts.vendorInfoCount; ++at)
            report += std::format("\n  {} (vendor code {:#x}, data {:#x})", vendor[at].description,
                vendor[at].vendorFaultCode, vendor[at].vendorFaultData);

        return report;
    }

    void Device::setNameImpl(VkObjectType type, std::uint64_t handle, const char* name) const
    {
        if (mSetObjectName == nullptr)
            return;

        const VkDebugUtilsObjectNameInfoEXT info{
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
            .objectType = type,
            .objectHandle = handle,
            .pObjectName = name,
        };

        // Deliberately unchecked. This is a label on a debugging aid, called from every resource
        // that gets created; a failure here must not be what stops a renderer that is otherwise
        // working, and the only documented failure is host memory exhaustion, which will announce
        // itself elsewhere within microseconds.
        mSetObjectName(mHandle, &info);
    }

    void Device::beginLabelImpl(VkCommandBuffer commands, const char* name) const
    {
        const VkDebugUtilsLabelEXT label{
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
            .pNext = nullptr,
            .pLabelName = name,
            // Left black, which every tool reads as "no colour was chosen" and picks its own. A
            // palette here would be one this fork maintains against tools that already have one.
            .color = { 0.0f, 0.0f, 0.0f, 0.0f },
        };

        mBeginLabel(commands, &label);
    }
}
