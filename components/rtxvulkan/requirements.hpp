#pragma once

#include <cstddef>
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
        /// names as two calls. Nothing here asks the extension to sort, because sorting was measured
        /// four ways and lost every one: a reorder point costs 17 to 23 per cent of the trace at
        /// every place, as much with no key as with one, and the launch is already 89 to 100 per
        /// cent coherent on the key before anyone sorts it — what diverges in a frame is the bounce,
        /// the lamp reservoir and the cutout loop, none of which a key can name.
        VkPhysicalDeviceRayTracingInvocationReorderFeaturesEXT mInvocationReorder{};

        /// What lets the driver be asked how it compiled a pipeline: registers a thread, spills,
        /// waves a multiprocessor. See `ComputePipeline`, which is where the answer is read.
        VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR mPipelineExecutable{};

        /// The device's real-time clock, read from a shader: what `StressPass` holds a queue
        /// against.
        VkPhysicalDeviceShaderClockFeaturesKHR mShaderClock{};
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

        /// The device's heaps and memory types, read once for the device that is chosen: the memory
        /// report names every heap in each report it makes (`Memory::report`).
        VkPhysicalDeviceMemoryProperties mMemory{};
    };

    /// A feature the renderer will not start without, and how to reach it in the chain, so one
    /// table both checks a device and asks for exactly the set that was checked.
    struct RequiredFeature
    {
        std::string_view mName;
        VkBool32& (*mField)(DeviceFeatures& features);
    };

    /// A format feature the renderer will not start without: the format, what it has to offer in
    /// optimal tiling, and what for. Vulkan makes most of what the frame writes mandatory and a
    /// few things optional, and a card that lacks an optional one is named for it here rather
    /// than found out by a validation message under the first texture.
    struct RequiredFormat
    {
        VkFormat mFormat;
        VkFormatFeatureFlags mFeatures;
        std::string_view mFor;
    };

    /// An extension the renderer will not start without, and the NVIDIA driver that first offers it
    /// on every RTX card where that driver is later than the first to report Vulkan 1.4 — empty where
    /// it is not. A card refused for such an extension is a driver update from running, and the
    /// refusal says so rather than reading as a limit of the hardware.
    struct RequiredExtension
    {
        const char* mName;
        std::string_view mNvidiaDriver;
    };

    std::span<const RequiredExtension> getRequiredDeviceExtensions();

    /// The table itself, in the order `PhysicalDevice::profileOf` reads the device's answers in.
    std::span<const RequiredFormat> getRequiredFormats();

    /// What the renderer does where the driver offers it and goes without where it does not.
    enum class DeviceOption : std::uint8_t
    {
        FaultReport,
        MemoryBudget,
        PresentFences,
        Checkpoints,
        Pacing,
    };

    inline constexpr std::size_t sDeviceOptions = static_cast<std::size_t>(DeviceOption::Pacing) + 1;

    /// One option as extensions: the ones it is made of, enabled all or none, and what the registry
    /// says has to be enabled beside them — an instance extension or a device one — before any of
    /// them may be. An option whose needs are not met is not taken, however much of it the driver
    /// offers: the present id rests on a swapchain, which a device with no window has none of.
    struct OptionalExtensions
    {
        DeviceOption mOption;
        std::span<const char* const> mExtensions;
        std::span<const char* const> mNeeds;
    };

    /// Every option, in the order of `DeviceOption`. Reported by `openmw-rtxtool info`, so it is
    /// visible which a device offers; `Device::has` says which a device took.
    std::span<const OptionalExtensions> getOptionalExtensions();

    /// The table itself, so a test can prove its entries address distinct fields.
    std::span<const RequiredFeature> getRequiredDeviceFeatures();

    /// Sets every required feature to `VK_TRUE`, leaving the rest alone.
    void requestRequiredFeatures(DeviceFeatures& features);

    /// Appends the name of each required feature `supported` lacks. Nothing is appended when the
    /// device qualifies. `supported` is mutable because the table's accessor serves both
    /// directions; nothing is written.
    void findMissingFeatures(DeviceFeatures& supported, std::vector<std::string_view>& missing);
}
