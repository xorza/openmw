#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "physicaldevice.hpp"

namespace Rtx
{
    class Instance;
    class MemoryAllocator;
    class PipelineCache;
    struct PipelineCacheSpec;

    /// Entry points that come from the required extensions rather than from core Vulkan, resolved
    /// and checked once at device creation, so a driver that advertises an extension it cannot
    /// dispatch fails at startup.
    struct DeviceFunctions
    {
        PFN_vkGetAccelerationStructureBuildSizesKHR mGetAccelerationStructureBuildSizes = nullptr;
        PFN_vkCreateAccelerationStructureKHR mCreateAccelerationStructure = nullptr;
        PFN_vkDestroyAccelerationStructureKHR mDestroyAccelerationStructure = nullptr;
        PFN_vkCmdBuildAccelerationStructuresKHR mCmdBuildAccelerationStructures = nullptr;

        /// What a structure would come to if it were copied tight. `SceneAcceleration` writes the
        /// answers into a query pool after a build and reports their sum.
        PFN_vkCmdWriteAccelerationStructuresPropertiesKHR mCmdWriteAccelerationStructuresProperties = nullptr;

        /// The copy those answers are for, which is the only way a structure is made tight.
        PFN_vkCmdCopyAccelerationStructureKHR mCmdCopyAccelerationStructure = nullptr;

        PFN_vkGetAccelerationStructureDeviceAddressKHR mGetAccelerationStructureDeviceAddress = nullptr;

        PFN_vkCreateRayTracingPipelinesKHR mCreateRayTracingPipelines = nullptr;
        PFN_vkGetRayTracingShaderGroupHandlesKHR mGetRayTracingShaderGroupHandles = nullptr;
        PFN_vkCmdTraceRaysKHR mCmdTraceRays = nullptr;

        PFN_vkGetPipelineExecutablePropertiesKHR mGetPipelineExecutableProperties = nullptr;
        PFN_vkGetPipelineExecutableStatisticsKHR mGetPipelineExecutableStatistics = nullptr;
    };

    /// A logical device, its single queue, and the extension entry points.
    class Device
    {
    public:
        /// @param instance must outlive the device. Not held: a `VkDevice` does not reference its
        ///        instance, but every entry point reached through it does.
        /// @param cache where the pipeline cache is kept and what it is keyed on. An empty
        ///        directory keeps none, and every pipeline is compiled from source every run.
        /// @param extraExtensions device extensions beyond the required and optional lists — the
        ///        swapchain, when there is a window.
        Device(const Instance& instance, PhysicalDevice&& physicalDevice, const PipelineCacheSpec& cache,
            const std::vector<const char*>& extraExtensions = {});
        ~Device();

        VkDevice getHandle() const { return mHandle; }
        VkQueue getQueue() const { return mQueue; }
        std::uint32_t getQueueFamily() const { return mPhysicalDevice.getQueueFamily(); }
        const PhysicalDevice& getPhysicalDevice() const { return mPhysicalDevice; }
        const DeviceFunctions& getFunctions() const { return mFunctions; }

        /// Where every buffer's and every image's memory comes from: one suballocator for the
        /// device, where an allocation per resource is a kernel call apiece for a cell's several
        /// hundred images. Not const although the device is, because every resource that asks holds
        /// the device by const reference.
        MemoryAllocator& getMemory() const;

        /// Whether `vkQueuePresentKHR` may be handed a fence it signals when the presentation
        /// engine has finished with an image — the only thing that says so, since a queue-idle
        /// proves the queue is empty and not that the compositor has let go.
        bool hasPresentFences() const { return mPresentFences; }

        /// Handed to every `vkCreate*Pipelines` on this device, so that a shader is compiled once
        /// per change rather than once per pipeline.
        VkPipelineCache getPipelineCache() const;

        /// Logs what the driver's compiler made of `pipeline` — registers a thread, spills, shared
        /// memory a block. The register count exists only inside the driver, which is what lets an
        /// occupancy figure be had here with no external profiler.
        void reportPipeline(VkPipeline pipeline, std::string_view name) const;

        /// Whether a name handed to `setName` or `beginLabel` reaches anything at all — what a
        /// caller asks before it builds one, or a release run spends a heap allocation per texture
        /// on a name nothing can read.
        static constexpr bool wantsNames()
        {
#ifdef OPENMW_RTX_DEBUG_NAMES
            return true;
#else
            return false;
#endif
        }

        /// Attaches a name to a Vulkan object so captures and validation messages name it. Compiled
        /// to nothing in release: an unreadable capture is a debugging session that does not
        /// happen, and a released build has no captures.
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
        /// Compiled to nothing in release.
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

        /// Whether the driver offers `VK_EXT_device_fault` with its feature, and so whether
        /// `describeFault` has anything to ask.
        bool canDescribeFault() const
        {
            return mGetDeviceFaultInfo != nullptr;
        }

        /// What the device says about why it was lost, as lines for the message that reports it.
        /// Nothing where the driver offers no `VK_EXT_device_fault`. After a loss and never before,
        /// because the extension forbids the question of a device that is still answering.
        std::string describeFault() const;

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

        /// Null where the driver offers no `VK_EXT_device_fault`, or offers the extension without
        /// its feature.
        PFN_vkGetDeviceFaultInfoEXT mGetDeviceFaultInfo = nullptr;

        bool mPresentFences = false;

        // Last, so that they are torn down first: saving the cache reads from the device, and
        // freeing a block writes to it, which the members above are still holding open at that
        // point.
        std::unique_ptr<PipelineCache> mPipelineCache;
        std::unique_ptr<MemoryAllocator> mMemory;
    };
}
