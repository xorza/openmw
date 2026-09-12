#include "requirements.hpp"

#include <array>
#include <string>

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
            VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
            VK_KHR_RAY_QUERY_EXTENSION_NAME,
            VK_KHR_RAY_TRACING_POSITION_FETCH_EXTENSION_NAME,
            VK_KHR_RAY_TRACING_MAINTENANCE_1_EXTENSION_NAME,
            VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
            // The launch, and what it is for. The trace calls `traceRayEXT` nowhere and still has to
            // be a ray tracing pipeline, because a hit object may only be traced from the ray
            // generation stage, and the shader it names may only be run from there.
            VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
            // A driver floor and not a hardware one: hit objects are this extension's and no launch
            // here sorts, but the shaders compile to `SPV_EXT_shader_invocation_reorder`, which
            // arrives around driver 595 on Turing and 582 on Ada. The `NV` extension carries the
            // `NV` SPIR-V capability, so taking it would mean a second binary of every shader.
            VK_EXT_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME,
            // Occupancy is a register count the driver's own compiler owns and no offline tool has.
            // Required, because a renderer that quietly reported nothing would be a fallback path.
            VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME,
        };

        constexpr std::array sOptionalDeviceExtensions{
            // Turns a device loss from "the driver said no" into where it faulted: the addresses,
            // how precisely they are known, and what the vendor adds. `Device::describeFault` is
            // what reads it, and `Device` enables its feature where the driver has it.
            VK_EXT_DEVICE_FAULT_EXTENSION_NAME,
            // What the driver says is left, which the heap's own size does not. A budget moves
            // with whatever else is on the card, and it is the figure a residency decision belongs
            // against — most of all on a card whose host-visible heap is a couple of hundred
            // megabytes. `MemoryAllocator::report` is what reads it.
            VK_EXT_MEMORY_BUDGET_EXTENSION_NAME,
            // A fence the presentation engine signals, which is the only thing that says it has
            // finished with an image. `Presenter` retires its semaphores and its swapchain against
            // one where the driver has it, and against a device-idle where it does not.
            VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME,
        };

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

            RequiredFeature{ "bufferDeviceAddress",
                +[](DeviceFeatures& f) -> VkBool32& { return f.mVulkan12.bufferDeviceAddress; } },
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
            RequiredFeature{ "rayTracingInvocationReorder",
                +[](DeviceFeatures& f) -> VkBool32& { return f.mInvocationReorder.rayTracingInvocationReorder; } },
            RequiredFeature{ "pipelineExecutableInfo",
                +[](DeviceFeatures& f) -> VkBool32& { return f.mPipelineExecutable.pipelineExecutableInfo; } },
        };
    }

    DeviceFeatures::DeviceFeatures()
    {
        void* next = nullptr;
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

    std::span<const char* const> getRequiredDeviceExtensions()
    {
        return sRequiredDeviceExtensions;
    }

    std::span<const char* const> getOptionalDeviceExtensions()
    {
        return sOptionalDeviceExtensions;
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
