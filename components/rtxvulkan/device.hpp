#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <vulkan/vulkan_core.h>

#include "physicaldevice.hpp"

namespace Rtx
{
    class Instance;
    class PipelineCache;

    /// Entry points that come from the required extensions rather than from core Vulkan.
    ///
    /// Resolved once at device creation and missing-checked there, so a driver that advertises an
    /// extension it cannot actually dispatch fails at startup instead of at the first frame that
    /// needed it.
    struct DeviceFunctions
    {
        PFN_vkGetAccelerationStructureBuildSizesKHR mGetAccelerationStructureBuildSizes = nullptr;
        PFN_vkCreateAccelerationStructureKHR mCreateAccelerationStructure = nullptr;
        PFN_vkDestroyAccelerationStructureKHR mDestroyAccelerationStructure = nullptr;
        PFN_vkCmdBuildAccelerationStructuresKHR mCmdBuildAccelerationStructures = nullptr;
        PFN_vkCmdCopyAccelerationStructureKHR mCmdCopyAccelerationStructure = nullptr;
        PFN_vkCmdWriteAccelerationStructuresPropertiesKHR mCmdWriteAccelerationStructuresProperties = nullptr;
        PFN_vkGetAccelerationStructureDeviceAddressKHR mGetAccelerationStructureDeviceAddress = nullptr;

        PFN_vkCreateRayTracingPipelinesKHR mCreateRayTracingPipelines = nullptr;
        PFN_vkGetRayTracingShaderGroupHandlesKHR mGetRayTracingShaderGroupHandles = nullptr;
        PFN_vkCmdTraceRaysKHR mCmdTraceRays = nullptr;

        PFN_vkCreateMicromapEXT mCreateMicromap = nullptr;
        PFN_vkDestroyMicromapEXT mDestroyMicromap = nullptr;
        PFN_vkGetMicromapBuildSizesEXT mGetMicromapBuildSizes = nullptr;
        PFN_vkCmdBuildMicromapsEXT mCmdBuildMicromaps = nullptr;
        PFN_vkGetPipelineExecutablePropertiesKHR mGetPipelineExecutableProperties = nullptr;
        PFN_vkGetPipelineExecutableStatisticsKHR mGetPipelineExecutableStatistics = nullptr;
    };

    /// A logical device, its single queue, and the extension entry points.
    class Device
    {
    public:
        /// @param instance must outlive the device. Not held: a `VkDevice` does not reference its
        ///        instance, but every entry point reached through it does.
        /// @param extraExtensions device extensions beyond the required and optional lists — the
        ///        swapchain, when there is a window.
        Device(const Instance& instance, PhysicalDevice&& physicalDevice,
            const std::vector<const char*>& extraExtensions = {});
        ~Device();

        Device(const Device&) = delete;
        Device& operator=(const Device&) = delete;

        VkDevice getHandle() const { return mHandle; }
        VkQueue getQueue() const { return mQueue; }
        std::uint32_t getQueueFamily() const { return mPhysicalDevice.getQueueFamily(); }
        const PhysicalDevice& getPhysicalDevice() const { return mPhysicalDevice; }
        const DeviceFunctions& getFunctions() const { return mFunctions; }

        /// Handed to every `vkCreate*Pipelines` on this device, so that a shader is compiled once
        /// per change rather than once per pipeline.
        ///
        /// Out of line because the cache is only forward-declared here: it carries `<filesystem>`,
        /// and this header is included by two dozen others that have no use for it.
        VkPipelineCache getPipelineCache() const;

        /// Logs what the driver's compiler made of `pipeline` — registers a thread, spills, shared
        /// memory a block, or whatever else it chose to report.
        ///
        /// **An occupancy figure is a register count and a workgroup size**, and the register count
        /// exists only inside the driver: no offline compiler has it, because the allocation is the
        /// driver's — which is what lets an occupancy figure be had here with no external profiler.
        ///
        /// **Here rather than on either pipeline class**, because it is the device's compiler being
        /// asked and both classes ask it: the entry points are already members, and a copy apiece
        /// would be two places for the format switch to fall behind a driver.
        void reportPipeline(VkPipeline pipeline, std::string_view name) const;

        /// Attaches a name to a Vulkan object so captures and validation messages name it.
        ///
        /// Compiled to nothing in release: an unreadable capture is a debugging session that does
        /// not happen, and a released build has no captures.
        void setName([[maybe_unused]] VkObjectType type, [[maybe_unused]] std::uint64_t handle,
            [[maybe_unused]] std::string_view name) const
        {
#ifdef OPENMW_RTX_DEBUG_NAMES
            // Terminated here and nowhere else, so a release build constructs nothing at all — and a
            // caller may hand over a literal or a view into a path it is already holding.
            setNameImpl(type, handle, std::string(name).c_str());
#endif
        }

        /// Opens a named region in `commands`, so a capture shows what each stretch of the frame is.
        ///
        /// **The half of a performance picture a timestamp cannot give you.** A number says the
        /// trace cost six milliseconds; an external profiler says which of its rays missed cache,
        /// and it can only say so about a region somebody named. Compiled to nothing in release, for
        /// the reason `setName` gives.
        void beginLabel([[maybe_unused]] VkCommandBuffer commands, [[maybe_unused]] std::string_view name) const
        {
#ifdef OPENMW_RTX_DEBUG_NAMES
            if (mBeginLabel != nullptr)
                beginLabelImpl(commands, std::string(name).c_str());
#endif
        }

        void endLabel([[maybe_unused]] VkCommandBuffer commands) const
        {
#ifdef OPENMW_RTX_DEBUG_NAMES
            if (mEndLabel != nullptr)
                mEndLabel(commands);
#endif
        }

        /// Blocks until the queue has finished everything. For tearing down and for resizing, not
        /// for pacing a frame.
        void waitIdle() const;

    private:
        void setNameImpl(VkObjectType type, std::uint64_t handle, const char* name) const;
        void beginLabelImpl(VkCommandBuffer commands, const char* name) const;

        PhysicalDevice mPhysicalDevice;
        VkDevice mHandle = VK_NULL_HANDLE;
        VkQueue mQueue = VK_NULL_HANDLE;
        DeviceFunctions mFunctions;
        PFN_vkSetDebugUtilsObjectNameEXT mSetObjectName = nullptr;
        PFN_vkCmdBeginDebugUtilsLabelEXT mBeginLabel = nullptr;
        PFN_vkCmdEndDebugUtilsLabelEXT mEndLabel = nullptr;

        // Last, so that it is torn down first: saving it reads from the device, which the members
        // above are still holding open at that point.
        std::unique_ptr<PipelineCache> mPipelineCache;
    };
}
