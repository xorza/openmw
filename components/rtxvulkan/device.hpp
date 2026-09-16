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
    class CommandPool;
    class Graveyard;
    class Instance;
    class MemoryAllocator;
    class Timeline;
    class PipelineCache;
    struct PipelineCacheSpec;

    /// Which `VkObjectType` a handle is, so that `Device::setName` takes the handle and never a
    /// type a caller could get wrong. Every handle this backend names is listed; one that is not is
    /// a compile error rather than a name filed under the wrong kind. The handles are distinct
    /// pointer types on the 64-bit targets this backend runs on, which is what lets them be told
    /// apart at all.
    template <class Handle>
    struct ObjectTypeOf;

    template <>
    struct ObjectTypeOf<VkBuffer>
    {
        static constexpr VkObjectType value = VK_OBJECT_TYPE_BUFFER;
    };

    template <>
    struct ObjectTypeOf<VkImage>
    {
        static constexpr VkObjectType value = VK_OBJECT_TYPE_IMAGE;
    };

    template <>
    struct ObjectTypeOf<VkImageView>
    {
        static constexpr VkObjectType value = VK_OBJECT_TYPE_IMAGE_VIEW;
    };

    template <>
    struct ObjectTypeOf<VkSampler>
    {
        static constexpr VkObjectType value = VK_OBJECT_TYPE_SAMPLER;
    };

    template <>
    struct ObjectTypeOf<VkPipeline>
    {
        static constexpr VkObjectType value = VK_OBJECT_TYPE_PIPELINE;
    };

    template <>
    struct ObjectTypeOf<VkShaderModule>
    {
        static constexpr VkObjectType value = VK_OBJECT_TYPE_SHADER_MODULE;
    };

    template <>
    struct ObjectTypeOf<VkSemaphore>
    {
        static constexpr VkObjectType value = VK_OBJECT_TYPE_SEMAPHORE;
    };

    template <>
    struct ObjectTypeOf<VkQueryPool>
    {
        static constexpr VkObjectType value = VK_OBJECT_TYPE_QUERY_POOL;
    };

    template <>
    struct ObjectTypeOf<VkAccelerationStructureKHR>
    {
        static constexpr VkObjectType value = VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR;
    };

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

    /// What a checkpoint on the queue points at: the zone the timer opened and the frame it was
    /// opened for, so a device loss can say "frame 83, `tlas`". Owned by the timer that set it
    /// and stable while the timer's slot lives, which is longer than a fault takes to be reported.
    struct Checkpoint
    {
        std::string_view mName;
        std::uint64_t mFrame = 0;
    };

    /// The logical device's own handle, with the destructor `Owned` cannot give it: a device is
    /// destroyed by `vkDestroyDevice(device, allocator)`, with no parent to name. A member declared
    /// before everything made on the device, so that whatever ends the `Device` — its destructor or
    /// a constructor that throws half way — destroys those first and this last, in the one order.
    class LogicalDevice
    {
    public:
        LogicalDevice() = default;
        ~LogicalDevice()
        {
            if (mHandle != VK_NULL_HANDLE)
                vkDestroyDevice(mHandle, nullptr);
        }

        LogicalDevice(const LogicalDevice&) = delete;
        LogicalDevice& operator=(const LogicalDevice&) = delete;

        VkDevice get() const { return mHandle; }

        /// Where `vkCreateDevice` puts one.
        VkDevice* put() { return &mHandle; }

    private:
        VkDevice mHandle = VK_NULL_HANDLE;
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

        VkDevice getHandle() const { return mHandle.get(); }
        VkQueue getQueue() const { return mQueue; }
        std::uint32_t getQueueFamily() const { return mPhysicalDevice.getQueueFamily(); }
        const PhysicalDevice& getPhysicalDevice() const { return mPhysicalDevice; }
        const DeviceFunctions& getFunctions() const { return mFunctions; }

        /// Where every buffer's and every image's memory comes from: one suballocator for the
        /// device, where an allocation per resource is a kernel call apiece for a cell's several
        /// hundred images. Not const although the device is, because every resource that asks holds
        /// the device by const reference.
        MemoryAllocator& getMemory() const;

        /// The queue's clock, which every submit signals and every wait reads.
        Timeline& getTimeline() const { return *mTimeline; }

        /// The queue's one command pool, which every submit is made out of, and what the queue may
        /// still be reading, which everything on the frame path lets go of through. The queue's
        /// and not the renderer's, so whatever holds the device can submit and bury without being
        /// handed either: a pool and a graveyard threaded through every constructor was two
        /// references beside a third they could have been reached through.
        CommandPool& getPool() const { return *mPool; }
        Graveyard& getGraveyard() const { return *mGraveyard; }

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
        template <class Handle>
        void setName([[maybe_unused]] Handle handle, [[maybe_unused]] std::string_view name) const
        {
#ifdef OPENMW_RTX_DEBUG_NAMES
            // Terminated here and nowhere else, so a release build constructs nothing at all — and a
            // caller may hand over a literal or a view into a path it is already holding.
            setNameImpl(
                ObjectTypeOf<Handle>::value, reinterpret_cast<std::uint64_t>(handle), std::string(name).c_str());
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

        /// Marks the queue's progress with `checkpoint`, which the queue reports as the last one
        /// each stage passed if the device is lost. Compiled to nothing in release, like the
        /// labels, and nothing where the driver offers no `VK_NV_device_diagnostic_checkpoints`.
        void checkpoint([[maybe_unused]] VkCommandBuffer commands, [[maybe_unused]] const Checkpoint* checkpoint) const
        {
#ifdef OPENMW_RTX_DEBUG_NAMES
            if (mCmdSetCheckpoint != nullptr)
                mCmdSetCheckpoint(commands, checkpoint);
#endif
        }

        /// Blocks until the queue has signalled `value` on the timeline, and then lets go of what
        /// the queue can no longer be reading: the graveyard's burials and the pool's finished
        /// command buffers. The one way to wait, so that no wait is made without the collect a
        /// wait owes. `what` names the wait in the error a device that stops answering produces.
        void waitFor(std::uint64_t value, const char* what) const;

        /// Blocks until the queue has finished everything, tells the clock so, and collects as
        /// `waitFor` does. For tearing down and for resizing, not for pacing a frame.
        void waitIdle() const;

        /// Lets go of everything the graveyard and the pool hold, a burial stamped for a submit
        /// nobody has made included. For a queue nothing is on and nothing is recorded for — a
        /// drain and a teardown — which both assert.
        void collectIdle() const;

        /// Whether a device object a submit may have read can be destroyed now: the queue is idle,
        /// or the graveyard is freeing what the timeline has passed. Every other destruction of
        /// such an object is one under a submit still reading it, which is what the destructors of
        /// `Buffer`, `Image` and `AccelerationStructure` assert against, and so a `Texture`'s
        /// through its images. A buffer knows its last reader and asks a narrower question first;
        /// the rest carry no stamp, because a bottom level is read by every top-level build
        /// without being named again.
        bool mayDestroy() const;

        // Read by the tests and by nothing else.
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
        /// What every wait ends with: the graveyard's burials and the pool's finished command
        /// buffers, let go of as far as the clock now reaches.
        void collect() const;

        /// The last checkpoint each stage of the queue passed, as lines for the fault report.
        /// Nothing where the driver offers no checkpoints.
        std::string describeCheckpoints() const;

        void setNameImpl(VkObjectType type, std::uint64_t handle, const char* name) const;
        void beginLabelImpl(VkCommandBuffer commands, const char* name) const;

        PhysicalDevice mPhysicalDevice;

        /// Before every member made on it — `LogicalDevice` says why.
        LogicalDevice mHandle;
        VkQueue mQueue = VK_NULL_HANDLE;
        DeviceFunctions mFunctions;
        PFN_vkSetDebugUtilsObjectNameEXT mSetObjectName = nullptr;
        PFN_vkCmdBeginDebugUtilsLabelEXT mBeginLabel = nullptr;
        PFN_vkCmdEndDebugUtilsLabelEXT mEndLabel = nullptr;

        /// Null where the driver offers no `VK_EXT_device_fault`, or offers the extension without
        /// its feature.
        PFN_vkGetDeviceFaultInfoEXT mGetDeviceFaultInfo = nullptr;

        /// Null where the driver offers no `VK_NV_device_diagnostic_checkpoints`.
        PFN_vkCmdSetCheckpointNV mCmdSetCheckpoint = nullptr;
        PFN_vkGetQueueCheckpointDataNV mGetQueueCheckpointData = nullptr;

        bool mPresentFences = false;

        // Last, so that they are torn down first, and in this order, because a later one dies
        // earlier: the graveyard frees through the pool and gives memory back to the allocator,
        // the pool and the clock hold device objects, and saving the cache and freeing a block
        // both call on the device that `mHandle` closes last of all.
        std::unique_ptr<PipelineCache> mPipelineCache;
        std::unique_ptr<MemoryAllocator> mMemory;
        std::unique_ptr<Timeline> mTimeline;
        std::unique_ptr<CommandPool> mPool;
        std::unique_ptr<Graveyard> mGraveyard;
    };
}
