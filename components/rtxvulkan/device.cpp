#include "device.hpp"

#include <components/debug/debuglog.hpp>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include <components/rtx/error.hpp>

#include "dlss.hpp"
#include "instance.hpp"
#include "pipelinecache.hpp"
#include "result.hpp"

namespace Rtx
{
    namespace
    {
        template <class T>
        void load(VkDevice device, T& out, const char* name)
        {
            out = reinterpret_cast<T>(vkGetDeviceProcAddr(device, name));
            if (out == nullptr)
                throw Error(
                    std::string("the driver advertises the extension providing ") + name + " but does not dispatch it");
        }
    }

    Device::Device(
        const Instance& instance, PhysicalDevice&& physicalDevice, const std::vector<const char*>& extraExtensions)
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

        const float priority = 1.0f;
        const VkDeviceQueueCreateInfo queue{
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = mPhysicalDevice.getQueueFamily(),
            .queueCount = 1,
            .pQueuePriorities = &priority,
        };

        const VkDeviceCreateInfo createInfo{
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .pNext = &features.mFeatures2,
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
            load(mHandle, mFunctions.mCmdCopyAccelerationStructure, "vkCmdCopyAccelerationStructureKHR");
            load(mHandle, mFunctions.mCmdWriteAccelerationStructuresProperties,
                "vkCmdWriteAccelerationStructuresPropertiesKHR");
            load(mHandle, mFunctions.mGetAccelerationStructureDeviceAddress,
                "vkGetAccelerationStructureDeviceAddressKHR");
            load(mHandle, mFunctions.mCreateRayTracingPipelines, "vkCreateRayTracingPipelinesKHR");
            load(mHandle, mFunctions.mGetRayTracingShaderGroupHandles, "vkGetRayTracingShaderGroupHandlesKHR");
            load(mHandle, mFunctions.mCmdTraceRays, "vkCmdTraceRaysKHR");
            load(mHandle, mFunctions.mCreateMicromap, "vkCreateMicromapEXT");
            load(mHandle, mFunctions.mDestroyMicromap, "vkDestroyMicromapEXT");
            load(mHandle, mFunctions.mGetMicromapBuildSizes, "vkGetMicromapBuildSizesEXT");
            load(mHandle, mFunctions.mCmdBuildMicromaps, "vkCmdBuildMicromapsEXT");
            load(mHandle, mFunctions.mGetPipelineExecutableProperties, "vkGetPipelineExecutablePropertiesKHR");
            load(mHandle, mFunctions.mGetPipelineExecutableStatistics, "vkGetPipelineExecutableStatisticsKHR");

            if (instance.hasDebugUtils())
            {
                mSetObjectName = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
                    vkGetDeviceProcAddr(mHandle, "vkSetDebugUtilsObjectNameEXT"));
                mBeginLabel = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
                    vkGetDeviceProcAddr(mHandle, "vkCmdBeginDebugUtilsLabelEXT"));
                mEndLabel = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
                    vkGetDeviceProcAddr(mHandle, "vkCmdEndDebugUtilsLabelEXT"));
            }

            mPipelineCache
                = std::make_unique<PipelineCache>(mHandle, mPhysicalDevice.getProperties().mProperties2.properties);
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
        checkVk(vkDeviceWaitIdle(mHandle), "vkDeviceWaitIdle");
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
