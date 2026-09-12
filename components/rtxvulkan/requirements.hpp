#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace Rtx
{
    /// The Vulkan version the renderer is written against — a floor, not a negotiation, and push
    /// descriptors alone, core in 1.4, are worth it.
    inline constexpr std::uint32_t sApiVersion = VK_API_VERSION_1_4;

    /// A packed Vulkan version as `major.minor.patch`.
    std::string versionString(std::uint32_t version);

    /// Every feature structure the renderer touches, chained by the constructor. One type serves
    /// both directions — what a device offers and what the renderer asks for — so they cannot
    /// drift apart. Non-copyable because the `pNext` pointers refer to its own members.
    struct DeviceFeatures
    {
        DeviceFeatures();

        DeviceFeatures(const DeviceFeatures&) = delete;
        DeviceFeatures& operator=(const DeviceFeatures&) = delete;

        VkPhysicalDeviceFeatures2 mFeatures2{};
        VkPhysicalDeviceVulkan12Features mVulkan12{};
        VkPhysicalDeviceVulkan13Features mVulkan13{};
        VkPhysicalDeviceVulkan14Features mVulkan14{};
        VkPhysicalDeviceAccelerationStructureFeaturesKHR mAccelerationStructure{};
        VkPhysicalDeviceRayQueryFeaturesKHR mRayQuery{};
        VkPhysicalDeviceRayTracingPositionFetchFeaturesKHR mPositionFetch{};
        VkPhysicalDeviceRayTracingMaintenance1FeaturesKHR mRayTracingMaintenance1{};

        /// What lets the trace be a launch rather than a dispatch: the eye's ray goes through the
        /// pipeline to a closest-hit shader traversal picked, and every ray after it is an inline
        /// query inside that shader.
        VkPhysicalDeviceRayTracingPipelineFeaturesKHR mRayTracingPipeline{};

        /// Hit objects: what lets the launch trace a ray, hold what it found, and run the shader it
        /// names as two calls. Nothing here asks the extension to sort, and
        /// `.notes/rtx/gpu-performance.md` says what measuring that found.
        VkPhysicalDeviceRayTracingInvocationReorderFeaturesEXT mInvocationReorder{};

        /// What lets the driver be asked how it compiled a pipeline: registers a thread, spills,
        /// waves a multiprocessor. See `ComputePipeline`, which is where the answer is read.
        VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR mPipelineExecutable{};
    };

    /// The properties worth reporting or budgeting against: one chained query, and the memory
    /// layout beside it. Non-copyable for the same reason as `DeviceFeatures`.
    struct DeviceProperties
    {
        DeviceProperties();

        DeviceProperties(const DeviceProperties&) = delete;
        DeviceProperties& operator=(const DeviceProperties&) = delete;

        VkPhysicalDeviceProperties2 mProperties2{};
        VkPhysicalDeviceVulkan11Properties mVulkan11{};
        VkPhysicalDeviceVulkan12Properties mVulkan12{};
        VkPhysicalDeviceAccelerationStructurePropertiesKHR mAccelerationStructure{};

        /// The shader group handle's size and the two alignments a shader binding table is laid out
        /// against. `TracePipeline` is what reads them.
        VkPhysicalDeviceRayTracingPipelinePropertiesKHR mRayTracingPipeline{};

        /// The largest record index a hit object may name, which `openmw-rtxtool info` prints.
        VkPhysicalDeviceRayTracingInvocationReorderPropertiesEXT mInvocationReorder{};

        /// The device's heaps and memory types, read once for the device that is chosen, because
        /// `findMemoryType` runs on every allocation the renderer makes.
        VkPhysicalDeviceMemoryProperties mMemory{};
    };

    /// A feature the renderer will not start without, and how to reach it in the chain, so one
    /// table both checks a device and asks for exactly the set that was checked.
    struct RequiredFeature
    {
        std::string_view mName;
        VkBool32& (*mField)(DeviceFeatures& features);
    };

    std::span<const char* const> getRequiredDeviceExtensions();

    /// Extensions used when the driver offers them and lived without when it does not. Reported by
    /// `openmw-rtxtool info` so it is visible which of them a run actually had.
    std::span<const char* const> getOptionalDeviceExtensions();

    /// The table itself, so a test can prove its entries address distinct fields.
    std::span<const RequiredFeature> getRequiredDeviceFeatures();

    /// Sets every required feature to `VK_TRUE`, leaving the rest alone.
    void requestRequiredFeatures(DeviceFeatures& features);

    /// Appends the name of each required feature `supported` lacks. Nothing is appended when the
    /// device qualifies. `supported` is mutable because the table's accessor serves both
    /// directions; nothing is written.
    void findMissingFeatures(DeviceFeatures& supported, std::vector<std::string_view>& missing);
}
