#include "requirements.hpp"

#include <array>
#include <cstddef>
#include <string>

#include <components/rtx/shaders/shadingmap.h>

#include "formats.hpp"

namespace Rtx
{
    namespace
    {
        template <class T>
        void chain(void*& next, T& structure, VkStructureType type)
        {
            structure.sType = type;
            structure.pNext = next;
            next = &structure;
        }

        constexpr std::array sRequiredDeviceExtensions{
            RequiredExtension{ VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, {} },
            RequiredExtension{ VK_KHR_RAY_QUERY_EXTENSION_NAME, {} },
            RequiredExtension{ VK_KHR_RAY_TRACING_POSITION_FETCH_EXTENSION_NAME, {} },
            RequiredExtension{ VK_KHR_RAY_TRACING_MAINTENANCE_1_EXTENSION_NAME, {} },
            RequiredExtension{ VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME, {} },
            // The launch, and what it is for. The trace calls `traceRayEXT` nowhere and still has to
            // be a ray tracing pipeline, because a hit object may only be traced from the ray
            // generation stage, and the shader it names may only be run from there.
            RequiredExtension{ VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME, {} },
            // A driver floor and not a hardware one: hit objects are this extension's and no launch
            // here sorts, but the shaders compile to `SPV_EXT_shader_invocation_reorder`. Every
            // Windows and Linux report in the Vulkan Hardware Database lists it from 595.44 on, RTX
            // 20 to 50 alike, and none at 591.86 or before. The exceptions are pre-releases: 595.02
            // without it, a few from 580.94 with it — so a refusal names the branch, 595. Older
            // drivers offer the `NV` extension instead, which carries the `NV` SPIR-V capability,
            // so taking it would mean a second binary of each shader that holds a hit object.
            RequiredExtension{ VK_EXT_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME, "595" },
            // Occupancy is a register count the driver's own compiler owns and no offline tool has.
            // Required, because a renderer that quietly reported nothing would be a fallback path.
            RequiredExtension{ VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME, {} },
            // A clock a shader reads that runs at one rate whatever the card is clocked at, which
            // is what `stress.comp` holds a queue against. Required and not optional, because a
            // hold that fell back to a count would be the one that misses on the first frames of
            // every run — the frames the hold is for.
            RequiredExtension{ VK_KHR_SHADER_CLOCK_EXTENSION_NAME, {} },
            // The one fused multiply-add the specification rounds once and exactly, which is how the
            // build fuses what it pins (`Rtx::pinFloatArithmetic`); `fma()` of the GLSL set may be
            // two roundings or one. The Vulkan beta drivers carried it from 580.94; of the release
            // drivers, the Vulkan Hardware Database's reports list it from 595.02 on and not at
            // 591.86 — the same branch as hit objects above.
            RequiredExtension{ VK_KHR_SHADER_FMA_EXTENSION_NAME, "595" },
        };

        constexpr std::array sFaultReport{ VK_EXT_DEVICE_FAULT_EXTENSION_NAME };
        constexpr std::array sMemoryBudget{ VK_EXT_MEMORY_BUDGET_EXTENSION_NAME };
        constexpr std::array sPresentFences{ VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME };
        constexpr std::array sCheckpoints{ VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME };
        constexpr std::array sPacing{ VK_KHR_PRESENT_ID_EXTENSION_NAME, VK_NV_LOW_LATENCY_2_EXTENSION_NAME };

        /// Whether this build sets checkpoints, which it does where it names things.
#ifdef OPENMW_RTX_DEBUG_NAMES
        constexpr bool sSetsCheckpoints = true;
#else
        constexpr bool sSetsCheckpoints = false;
#endif

        /// What the registry says a swapchain's extensions rest on beside core 1.4: the swapchain
        /// itself, which a device takes only where its instance has a surface, and for a present
        /// fence the instance's half of swapchain maintenance as well.
        constexpr std::array sSwapchain{ VK_KHR_SWAPCHAIN_EXTENSION_NAME };
        constexpr std::array sMaintainedSwapchain{
            VK_KHR_SWAPCHAIN_EXTENSION_NAME,
            VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME,
        };

        constexpr std::array sOptionalExtensions{
            // Turns a device loss from "the driver said no" into where it faulted: the addresses,
            // how precisely they are known, and what the vendor adds. `Device::describeFault` is
            // what reads it, and `Device` enables its feature where the driver has it.
            OptionalExtensions{ DeviceOption::FaultReport, sFaultReport, {} },
            // What the driver says is left, which the heap's own size does not. A budget moves
            // with whatever else is on the card, and it is the figure a residency decision belongs
            // against — most of all on a card whose host-visible heap is a couple of hundred
            // megabytes. `MemoryAllocator` is what reads it.
            OptionalExtensions{ DeviceOption::MemoryBudget, sMemoryBudget, {} },
            // A fence the presentation engine signals, which is the only thing that says it has
            // finished with an image. `Presenter` retires its semaphores and its swapchain against
            // one where the driver has it, and against a device-idle where it does not.
            OptionalExtensions{ DeviceOption::PresentFences, sPresentFences, sMaintainedSwapchain },
            // A marker the queue remembers passing, so a device loss names the last zone each
            // stage reached rather than an address: `GpuTimer::open` sets one per zone in a build
            // that names things, and `Device::describeFault` reads them back.
            OptionalExtensions{ DeviceOption::Checkpoints, sCheckpoints, {}, sSetsCheckpoints },
            // The driver's frame pacing — Reflex — and the number each present carries so the
            // driver can tell one frame's markers from the next's. Both or neither: the pacing
            // needs the present id, and the id alone is a number nothing reads. `LatencyPacer` is
            // what uses them, and only where there is a window.
            OptionalExtensions{ DeviceOption::Pacing, sPacing, sSwapchain },
        };

        /// A table out of `DeviceOption`'s order is an option read as another.
        constexpr bool everyOptionInItsPlace()
        {
            for (std::size_t at = 0; at < sOptionalExtensions.size(); ++at)
                if (static_cast<std::size_t>(sOptionalExtensions[at].mOption) != at)
                    return false;

            return sOptionalExtensions.size() == sDeviceOptions;
        }

        static_assert(everyOptionInItsPlace(), "the optional extensions are not in the order of their options");

        constexpr std::array sRequiredDeviceFeatures{
            RequiredFeature{
                "shaderInt64", +[](DeviceFeatures& f) -> VkBool32& { return f.mFeatures2.features.shaderInt64; } },

            // What lets `composite.comp` read one binding that is two formats. The bounce it
            // composites is the trace's own channel where nothing denoised the frame and the
            // cascade's where something did, and those are `rgba32f` and `rgba16f` — so the shader
            // states no format at all and the load converts from whatever the view holds.
            RequiredFeature{ "shaderStorageImageReadWithoutFormat",
                +[](DeviceFeatures& f) -> VkBool32& {
                    return f.mFeatures2.features.shaderStorageImageReadWithoutFormat;
                } },

            // And the store's half: the trace writes the two radiance channels and the composite
            // writes the frame at whichever width the run chose — `Rtx::RadianceWidth` — so those
            // three declare no format either.
            RequiredFeature{ "shaderStorageImageWriteWithoutFormat",
                +[](DeviceFeatures& f) -> VkBool32& {
                    return f.mFeatures2.features.shaderStorageImageWriteWithoutFormat;
                } },

            RequiredFeature{ "bufferDeviceAddress",
                +[](DeviceFeatures& f) -> VkBool32& { return f.mVulkan12.bufferDeviceAddress; } },

            // The queue's one clock, `Rtx::Timeline`.
            RequiredFeature{
                "timelineSemaphore", +[](DeviceFeatures& f) -> VkBool32& { return f.mVulkan12.timelineSemaphore; } },
            RequiredFeature{
                "descriptorIndexing", +[](DeviceFeatures& f) -> VkBool32& { return f.mVulkan12.descriptorIndexing; } },
            RequiredFeature{ "runtimeDescriptorArray",
                +[](DeviceFeatures& f) -> VkBool32& { return f.mVulkan12.runtimeDescriptorArray; } },
            RequiredFeature{ "descriptorBindingPartiallyBound",
                +[](DeviceFeatures& f) -> VkBool32& { return f.mVulkan12.descriptorBindingPartiallyBound; } },
            RequiredFeature{ "descriptorBindingSampledImageUpdateAfterBind",
                +[](DeviceFeatures& f) -> VkBool32& {
                    return f.mVulkan12.descriptorBindingSampledImageUpdateAfterBind;
                } },
            RequiredFeature{ "shaderSampledImageArrayNonUniformIndexing",
                +[](DeviceFeatures& f) -> VkBool32& { return f.mVulkan12.shaderSampledImageArrayNonUniformIndexing; } },
            RequiredFeature{
                "scalarBlockLayout", +[](DeviceFeatures& f) -> VkBool32& { return f.mVulkan12.scalarBlockLayout; } },

            RequiredFeature{
                "synchronization2", +[](DeviceFeatures& f) -> VkBool32& { return f.mVulkan13.synchronization2; } },
            RequiredFeature{ "maintenance4", +[](DeviceFeatures& f) -> VkBool32& { return f.mVulkan13.maintenance4; } },
            // The GUI is the only thing here that rasterises, and it does so without a render pass
            // or a framebuffer object.
            RequiredFeature{
                "dynamicRendering", +[](DeviceFeatures& f) -> VkBool32& { return f.mVulkan13.dynamicRendering; } },

            RequiredFeature{ "maintenance5", +[](DeviceFeatures& f) -> VkBool32& { return f.mVulkan14.maintenance5; } },
            RequiredFeature{
                "pushDescriptor", +[](DeviceFeatures& f) -> VkBool32& { return f.mVulkan14.pushDescriptor; } },

            RequiredFeature{ "accelerationStructure",
                +[](DeviceFeatures& f) -> VkBool32& { return f.mAccelerationStructure.accelerationStructure; } },
            RequiredFeature{ "rayQuery", +[](DeviceFeatures& f) -> VkBool32& { return f.mRayQuery.rayQuery; } },
            RequiredFeature{ "rayTracingPositionFetch",
                +[](DeviceFeatures& f) -> VkBool32& { return f.mPositionFetch.rayTracingPositionFetch; } },
            RequiredFeature{ "rayTracingMaintenance1",
                +[](DeviceFeatures& f) -> VkBool32& { return f.mRayTracingMaintenance1.rayTracingMaintenance1; } },
            RequiredFeature{ "rayTracingPipeline",
                +[](DeviceFeatures& f) -> VkBool32& { return f.mRayTracingPipeline.rayTracingPipeline; } },
            // What `VK_PIPELINE_CREATE_RAY_TRACING_SKIP_AABBS_BIT_KHR` is gated on: the promise
            // every launch makes that no procedural geometry is anywhere in the scene.
            RequiredFeature{ "rayTraversalPrimitiveCulling",
                +[](DeviceFeatures& f) -> VkBool32& { return f.mRayTracingPipeline.rayTraversalPrimitiveCulling; } },
            RequiredFeature{ "rayTracingInvocationReorder",
                +[](DeviceFeatures& f) -> VkBool32& { return f.mInvocationReorder.rayTracingInvocationReorder; } },
            RequiredFeature{ "pipelineExecutableInfo",
                +[](DeviceFeatures& f) -> VkBool32& { return f.mPipelineExecutable.pipelineExecutableInfo; } },
            RequiredFeature{
                "shaderDeviceClock", +[](DeviceFeatures& f) -> VkBool32& { return f.mShaderClock.shaderDeviceClock; } },
            RequiredFeature{
                "shaderFmaFloat32", +[](DeviceFeatures& f) -> VkBool32& { return f.mShaderFma.shaderFmaFloat32; } },
        };
    }

    namespace
    {
        constexpr std::array<RequiredFormat, 1> sRequiredFormats{
            // The shading estimate is written by a dispatch straight into the sixteen-bit map the
            // trace samples — `ShadingPass` — and a sixteen-bit unorm as a storage image is one of
            // the features Vulkan leaves optional.
            RequiredFormat{
                toVulkanFormat(SHADING_MAP_FORMAT), VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT, "the shading map's dispatch" },
        };
    }

    std::span<const RequiredFormat> getRequiredFormats()
    {
        return sRequiredFormats;
    }

    DeviceFeatures::DeviceFeatures()
    {
        void* next = nullptr;
        chain(next, mShaderFma, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FMA_FEATURES_KHR);
        chain(next, mShaderClock, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CLOCK_FEATURES_KHR);
        chain(next, mPipelineExecutable, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR);
        chain(next, mInvocationReorder, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_FEATURES_EXT);
        chain(next, mRayTracingPipeline, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR);
        chain(next, mRayTracingMaintenance1, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_MAINTENANCE_1_FEATURES_KHR);
        chain(next, mPositionFetch, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_POSITION_FETCH_FEATURES_KHR);
        chain(next, mRayQuery, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR);
        chain(next, mAccelerationStructure, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR);
        chain(next, mVulkan14, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES);
        chain(next, mVulkan13, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
        chain(next, mVulkan12, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
        chain(next, mFeatures2, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
    }

    DeviceProperties::DeviceProperties()
    {
        void* next = nullptr;
        chain(
            next, mInvocationReorder, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_PROPERTIES_EXT);
        chain(next, mRayTracingPipeline, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR);
        chain(next, mAccelerationStructure, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR);
        chain(next, mVulkan12, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES);
        chain(next, mVulkan11, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES);
        chain(next, mProperties2, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2);
    }

    std::span<const RequiredExtension> getRequiredDeviceExtensions()
    {
        return sRequiredDeviceExtensions;
    }

    std::span<const OptionalExtensions> getOptionalExtensions()
    {
        return sOptionalExtensions;
    }

    std::string versionString(std::uint32_t version)
    {
        return std::to_string(VK_API_VERSION_MAJOR(version)) + '.' + std::to_string(VK_API_VERSION_MINOR(version)) + '.'
            + std::to_string(VK_API_VERSION_PATCH(version));
    }

    std::span<const RequiredFeature> getRequiredDeviceFeatures()
    {
        return sRequiredDeviceFeatures;
    }

    void requestRequiredFeatures(DeviceFeatures& features)
    {
        for (const RequiredFeature& required : sRequiredDeviceFeatures)
            required.mField(features) = VK_TRUE;
    }

    void findMissingFeatures(DeviceFeatures& supported, std::vector<std::string_view>& missing)
    {
        for (const RequiredFeature& required : sRequiredDeviceFeatures)
            if (required.mField(supported) == VK_FALSE)
                missing.push_back(required.mName);
    }
}
