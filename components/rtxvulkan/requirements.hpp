#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace Rtx
{
    /// The Vulkan version the renderer is written against.
    ///
    /// A floor, not a negotiation. The target is Ada-class NVIDIA, which has been shipping 1.4 for
    /// a long time, and push descriptors alone — core in 1.4 — are worth it: per-pass bindings with
    /// no descriptor set to allocate is one fewer thing standing between a frame and zero
    /// allocations.
    inline constexpr std::uint32_t sApiVersion = VK_API_VERSION_1_4;

    /// A packed Vulkan version as `major.minor.patch`.
    std::string versionString(std::uint32_t version);

    /// Every feature structure the renderer touches, chained by the constructor.
    ///
    /// One type serves both directions: `vkGetPhysicalDeviceFeatures2` fills it in with what a
    /// device offers, and `VkDeviceCreateInfo::pNext` reads it as what the renderer is asking for.
    /// They cannot drift apart, which is the whole point.
    ///
    /// Non-copyable because the `pNext` pointers refer to its own members.
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
        VkPhysicalDeviceRayTracingPipelineFeaturesKHR mRayTracingPipeline{};
        VkPhysicalDeviceRayQueryFeaturesKHR mRayQuery{};
        VkPhysicalDeviceRayTracingPositionFetchFeaturesKHR mPositionFetch{};
        VkPhysicalDeviceRayTracingMaintenance1FeaturesKHR mRayTracingMaintenance1{};
        VkPhysicalDeviceOpacityMicromapFeaturesEXT mOpacityMicromap{};

        /// What lets the driver be asked how it compiled a pipeline: registers a thread, spills,
        /// waves a multiprocessor. See `ComputePipeline`, which is where the answer is read.
        VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR mPipelineExecutable{};
        VkPhysicalDeviceRayTracingInvocationReorderFeaturesEXT mInvocationReorder{};
    };

    /// The properties worth reporting or budgeting against, read in one chained query.
    ///
    /// Non-copyable for the same reason as `DeviceFeatures`.
    struct DeviceProperties
    {
        DeviceProperties();

        DeviceProperties(const DeviceProperties&) = delete;
        DeviceProperties& operator=(const DeviceProperties&) = delete;

        VkPhysicalDeviceProperties2 mProperties2{};
        VkPhysicalDeviceVulkan11Properties mVulkan11{};
        VkPhysicalDeviceVulkan12Properties mVulkan12{};
        VkPhysicalDeviceAccelerationStructurePropertiesKHR mAccelerationStructure{};
        VkPhysicalDeviceRayTracingPipelinePropertiesKHR mRayTracingPipeline{};
        VkPhysicalDeviceOpacityMicromapPropertiesEXT mOpacityMicromap{};
        VkPhysicalDeviceRayTracingInvocationReorderPropertiesEXT mInvocationReorder{};
    };

    /// A feature the renderer will not start without, and how to reach it in the chain.
    ///
    /// The accessor exists so one table drives both directions: reading it off a queried
    /// `DeviceFeatures` says whether a device qualifies, and writing it into a fresh one asks for
    /// exactly the set that was checked.
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
    /// device qualifies.
    ///
    /// `supported` is taken by mutable reference because the table reaches its fields through one
    /// accessor that both directions share; nothing is written.
    void findMissingFeatures(DeviceFeatures& supported, std::vector<std::string_view>& missing);
}
