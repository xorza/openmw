#include "device.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <components/debug/debuglog.hpp>
#include <components/rtx/error.hpp>

#include "commands.hpp"
#include "graveyard.hpp"
#include "instance.hpp"
#include "memory.hpp"
#include "pipelinecache.hpp"
#include "requirements.hpp"
#include "result.hpp"
#include "timeline.hpp"
#include "upscaler.hpp"

namespace Rtx
{
    namespace
    {
        /// Which stage a checkpoint was reported for, for the handful a queue reports on and
        /// the number for the rest.
        std::string checkpointStageName(const VkPipelineStageFlagBits stage)
        {
            switch (stage)
            {
                case VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT:
                    return "the top of the pipe";
                case VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT:
                    return "the bottom of the pipe";
                case VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT:
                    return "the compute stage";
                case VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR:
                    return "the ray tracing stage";
                case VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR:
                    return "the structure build stage";
                case VK_PIPELINE_STAGE_TRANSFER_BIT:
                    return "the transfer stage";
                default:
                    return std::format("stage {:#x}", static_cast<std::uint32_t>(stage));
            }
        }

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
        {
            // The swapchain half rests on the surface half, which is an instance extension: a
            // headless run loads neither, and a device that asked for this one without it is a
            // device the driver may refuse.
            if (std::strcmp(name, VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME) == 0
                && !instance.hasSurfaceMaintenance())
                continue;

            extensions.push_back(name);
        }
        // What the upscaler's runtime asks for, which it will not start without. Appended rather
        // than added to the required list because that list is what this renderer needs to trace
        // at all, and a build without an upscaler must not fail on a device that lacks them.
        for (const char* const name : upscalerDeviceExtensions())
        {
            // `VK_EXT_buffer_device_address` cannot come along, and not because it is missing:
            // the feature it provides is Vulkan 1.2 core here, enabled through
            // `VkPhysicalDeviceVulkan12Features`, and the spec forbids asking for both. NGX names
            // the pre-1.2 spelling because it supports drivers older than this one does.
            if (std::strcmp(name, VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME) == 0)
                continue;

            const auto already = [&](const char* const listed) { return std::strcmp(listed, name) == 0; };
            if (std::none_of(extensions.begin(), extensions.end(), already))
                extensions.push_back(name);
        }

        for (const char* const name : extraExtensions)
            extensions.push_back(name);

        DeviceFeatures features;
        requestRequiredFeatures(features);

        // Outside `DeviceFeatures`, because these are the features that are optional. That type
        // is what the renderer requires, asked and enabled as one list, and a feature it can do
        // without has no place in a list a device is refused for lacking.
        VkPhysicalDeviceFaultFeaturesEXT fault{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT };
        VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR presentFences{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR,
        };

        const bool offersFault = mPhysicalDevice.hasOptionalExtension(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
        const bool offersPresentFences = instance.hasSurfaceMaintenance()
            && mPhysicalDevice.hasOptionalExtension(VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);

        // Only what the device offers is chained, into the query and into the creation alike. A
        // driver may offer an extension without the feature it provides, so each has to be asked;
        // asking for one whose extension is not there is a structure the driver was never told to
        // expect.
        void* asked = nullptr;
        const auto chain = [&asked](auto& structure) {
            structure.pNext = asked;
            asked = &structure;
        };

        if (offersFault)
            chain(fault);
        if (offersPresentFences)
            chain(presentFences);

        if (asked != nullptr)
        {
            VkPhysicalDeviceFeatures2 offered{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = asked };
            vkGetPhysicalDeviceFeatures2(mPhysicalDevice.getHandle(), &offered);

            // The vendor binary is not asked for: nothing here could read it, and a feature enabled
            // for nothing is a feature to explain.
            fault.deviceFaultVendorBinary = VK_FALSE;
        }

        const bool describesFault = offersFault && fault.deviceFault == VK_TRUE;
        mPresentFences = offersPresentFences && presentFences.swapchainMaintenance1 == VK_TRUE;

        // The same chain again, of what the device turned out to have rather than what it offered,
        // in front of the features the renderer requires.
        asked = &features.mFeatures2;
        if (describesFault)
            chain(fault);
        if (mPresentFences)
            chain(presentFences);

        const float priority = 1.0f;
        const VkDeviceQueueCreateInfo queue{
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = mPhysicalDevice.getQueueFamily(),
            .queueCount = 1,
            .pQueuePriorities = &priority,
        };

        const VkDeviceCreateInfo createInfo{
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .pNext = asked,
            .queueCreateInfoCount = 1,
            .pQueueCreateInfos = &queue,
            .enabledExtensionCount = static_cast<std::uint32_t>(extensions.size()),
            .ppEnabledExtensionNames = extensions.data(),
            // Superseded by the VkPhysicalDeviceFeatures2 in the chain, and the two cannot both be set.
            .pEnabledFeatures = nullptr,
        };

        checkVk(vkCreateDevice(mPhysicalDevice.getHandle(), &createInfo, nullptr, mHandle.put()), "vkCreateDevice");

        // From here on a throw — a driver that advertises an extension it cannot dispatch, which
        // a load below reports — destroys the members already made, in reverse, and the device's
        // own handle last of all, which is what `LogicalDevice` is for.
        vkGetDeviceQueue(mHandle.get(), mPhysicalDevice.getQueueFamily(), 0, &mQueue);

        load(mHandle.get(), mFunctions.mGetAccelerationStructureBuildSizes, "vkGetAccelerationStructureBuildSizesKHR");
        load(mHandle.get(), mFunctions.mCreateAccelerationStructure, "vkCreateAccelerationStructureKHR");
        load(mHandle.get(), mFunctions.mDestroyAccelerationStructure, "vkDestroyAccelerationStructureKHR");
        load(mHandle.get(), mFunctions.mCmdBuildAccelerationStructures, "vkCmdBuildAccelerationStructuresKHR");
        load(mHandle.get(), mFunctions.mCmdWriteAccelerationStructuresProperties,
            "vkCmdWriteAccelerationStructuresPropertiesKHR");
        load(mHandle.get(), mFunctions.mCmdCopyAccelerationStructure, "vkCmdCopyAccelerationStructureKHR");
        load(mHandle.get(), mFunctions.mGetAccelerationStructureDeviceAddress,
            "vkGetAccelerationStructureDeviceAddressKHR");
        load(mHandle.get(), mFunctions.mCreateRayTracingPipelines, "vkCreateRayTracingPipelinesKHR");
        load(mHandle.get(), mFunctions.mGetRayTracingShaderGroupHandles, "vkGetRayTracingShaderGroupHandlesKHR");
        load(mHandle.get(), mFunctions.mCmdTraceRays, "vkCmdTraceRaysKHR");
        load(mHandle.get(), mFunctions.mGetPipelineExecutableProperties, "vkGetPipelineExecutablePropertiesKHR");
        load(mHandle.get(), mFunctions.mGetPipelineExecutableStatistics, "vkGetPipelineExecutableStatisticsKHR");

        if (describesFault)
            load(mHandle.get(), mGetDeviceFaultInfo, "vkGetDeviceFaultInfoEXT");

        if (mPhysicalDevice.hasOptionalExtension(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME))
        {
            load(mHandle.get(), mCmdSetCheckpoint, "vkCmdSetCheckpointNV");
            load(mHandle.get(), mGetQueueCheckpointData, "vkGetQueueCheckpointDataNV");
        }

        if (instance.hasDebugUtils())
        {
            mSetObjectName = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
                vkGetDeviceProcAddr(mHandle.get(), "vkSetDebugUtilsObjectNameEXT"));
            mBeginLabel = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
                vkGetDeviceProcAddr(mHandle.get(), "vkCmdBeginDebugUtilsLabelEXT"));
            mEndLabel = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
                vkGetDeviceProcAddr(mHandle.get(), "vkCmdEndDebugUtilsLabelEXT"));
        }

        mPipelineCache = std::make_unique<PipelineCache>(
            mHandle.get(), mPhysicalDevice.getProperties().mProperties2.properties, cache);
        mMemory = std::make_unique<MemoryAllocator>(instance.getHandle(), mPhysicalDevice.getHandle(), mHandle.get(),
            mPhysicalDevice.getProperties().mMemory,
            mPhysicalDevice.hasOptionalExtension(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME));
        mTimeline = std::make_unique<Timeline>(*this);
        // `new` and not `make_unique`, which the pool's private constructor does not admit.
        mPool.reset(new CommandPool(*this));
        mGraveyard = std::make_unique<Graveyard>(*this);
    }

    Device::~Device()
    {
        // Through the wrapper and so through `tearDown`, which is what keeps the fault description
        // a lost device carries: the raw call answered with a number nobody read. The members
        // then go in the order they are declared for.
        tearDown("the device would not finish before it was destroyed", [&] { waitIdle(); });
    }

    MemoryAllocator& Device::getMemory() const
    {
        return *mMemory;
    }

    VkPipelineCache Device::getPipelineCache() const
    {
        return mPipelineCache->getHandle();
    }

    void Device::reportPipeline(VkPipeline pipeline, std::string_view name, const std::optional<double> compileMs) const
    {
        if (compileMs.has_value())
            Log(Debug::Verbose) << "pipeline " << name << ": compiled in " << *compileMs << " ms";

        const VkPipelineInfoKHR asked{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR,
            .pipeline = pipeline,
        };

        std::uint32_t executables = 0;
        checkVk(mFunctions.mGetPipelineExecutableProperties(mHandle.get(), &asked, &executables, nullptr),
            "vkGetPipelineExecutablePropertiesKHR");

        // Said once per pipeline rather than left as a missing line: NVIDIA's compiler reports no
        // executable at all for a ray tracing pipeline, and no internal representation for any, so
        // a reader could not otherwise tell it from a call nobody made. Nsight Graphics is where
        // that is read.
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

            const std::vector<VkPipelineExecutableStatisticKHR> statistics
                = enumerateVk<VkPipelineExecutableStatisticKHR>(
                    "vkGetPipelineExecutableStatisticsKHR",
                    [&](std::uint32_t* count, VkPipelineExecutableStatisticKHR* into) {
                        return mFunctions.mGetPipelineExecutableStatistics(mHandle.get(), &which, count, into);
                    },
                    VkPipelineExecutableStatisticKHR{ .sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR });

            // Whatever the driver chose to say, and not a list this side picked. The names are
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

    void Device::waitFor(const std::uint64_t value, const char* const what) const
    {
        mTimeline->waitFor(value, what);
        collect();
    }

    void Device::waitIdle() const
    {
        checkVk(*this, vkDeviceWaitIdle(mHandle.get()), "vkDeviceWaitIdle");
        mTimeline->markIdle();
        collect();
    }

    void Device::collect() const
    {
        mGraveyard->collect();
        mPool->collect();
    }

    void Device::collectIdle() const
    {
        mGraveyard->collectIdle();
        mPool->collectIdle();
    }

    std::string Device::describeCheckpoints() const
    {
        if (mGetQueueCheckpointData == nullptr)
            return {};

        const std::vector<VkCheckpointDataNV> passed = enumerateVk<VkCheckpointDataNV>(
            "vkGetQueueCheckpointDataNV",
            [&](std::uint32_t* count, VkCheckpointDataNV* into) {
                mGetQueueCheckpointData(mQueue, count, into);
                return VK_SUCCESS;
            },
            VkCheckpointDataNV{ .sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV });
        if (passed.empty())
            return "\nthe queue passed no checkpoint";

        std::string report;
        for (std::size_t at = 0; at < passed.size(); ++at)
        {
            const auto* checkpoint = static_cast<const Checkpoint*>(passed[at].pCheckpointMarker);
            if (checkpoint == nullptr)
                continue;

            report += std::format("\n  {} last passed `{}` of frame {}", checkpointStageName(passed[at].stage),
                checkpoint->mName, checkpoint->mFrame);
        }

        return report;
    }

    std::string Device::describeFault() const
    {
        if (mGetDeviceFaultInfo == nullptr)
            return describeCheckpoints();

        constexpr const char* sUnsaid = "\nthe driver would not say where the device faulted";

        VkDeviceFaultCountsEXT counts{ .sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT };
        if (mGetDeviceFaultInfo(mHandle.get(), &counts, nullptr) != VK_SUCCESS)
            return sUnsaid + describeCheckpoints();

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
        const VkResult result = mGetDeviceFaultInfo(mHandle.get(), &counts, &info);
        if (result != VK_SUCCESS && result != VK_INCOMPLETE)
            return sUnsaid + describeCheckpoints();

        std::string report = "\ndevice fault: ";
        report += info.description;
        for (std::uint32_t at = 0; at < counts.addressInfoCount; ++at)
            report += std::format("\n  {} at {:#x}, known to within {:#x}",
                faultAddressTypeName(addresses[at].addressType), addresses[at].reportedAddress,
                addresses[at].addressPrecision);
        for (std::uint32_t at = 0; at < counts.vendorInfoCount; ++at)
            report += std::format("\n  {} (vendor code {:#x}, data {:#x})", vendor[at].description,
                vendor[at].vendorFaultCode, vendor[at].vendorFaultData);

        return report + describeCheckpoints();
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
        mSetObjectName(mHandle.get(), &info);
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
