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
            VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
            VK_KHR_RAY_QUERY_EXTENSION_NAME,
            VK_KHR_RAY_TRACING_POSITION_FETCH_EXTENSION_NAME,
            VK_KHR_RAY_TRACING_MAINTENANCE_1_EXTENSION_NAME,
            VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
            VK_EXT_OPACITY_MICROMAP_EXTENSION_NAME,
            VK_EXT_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME,
            // **Asking the driver what it made of a shader.** Occupancy is a register count and a
            // workgroup size, and the register count belongs to the driver's own compiler — no
            // offline tool has it. Required rather than optional because a device that will not say
            // is a device this fork cannot answer 4.6 on, and a renderer that quietly reported
            // nothing would be the fallback path this tree does not keep.
            VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME,
        };

        constexpr std::array sOptionalDeviceExtensions{
            // The in-game path hands its image to OpenGL rather than to a swapchain.
            VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
            VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
            // Turns a device loss from "the driver said no" into a list of what had not completed.
            VK_EXT_DEVICE_FAULT_EXTENSION_NAME,
            // Weighed at M12, not before.
            VK_NV_CLUSTER_ACCELERATION_STRUCTURE_EXTENSION_NAME,
            VK_NV_PARTITIONED_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        };

        constexpr std::array sRequiredDeviceFeatures{
            RequiredFeature{
                "shaderInt64", +[](DeviceFeatures& f) -> VkBool32& { return f.mFeatures2.features.shaderInt64; } },
            RequiredFeature{ "samplerAnisotropy",
                +[](DeviceFeatures& f) -> VkBool32& { return f.mFeatures2.features.samplerAnisotropy; } },

            RequiredFeature{ "bufferDeviceAddress",
                +[](DeviceFeatures& f) -> VkBool32& { return f.mVulkan12.bufferDeviceAddress; } },
            RequiredFeature{
                "descriptorIndexing", +[](DeviceFeatures& f) -> VkBool32& { return f.mVulkan12.descriptorIndexing; } },
            RequiredFeature{ "runtimeDescriptorArray",
                +[](DeviceFeatures& f) -> VkBool32& { return f.mVulkan12.runtimeDescriptorArray; } },
            RequiredFeature{ "descriptorBindingPartiallyBound",
                +[](DeviceFeatures& f) -> VkBool32& { return f.mVulkan12.descriptorBindingPartiallyBound; } },
            RequiredFeature{ "descriptorBindingVariableDescriptorCount",
                +[](DeviceFeatures& f) -> VkBool32& { return f.mVulkan12.descriptorBindingVariableDescriptorCount; } },
            RequiredFeature{ "descriptorBindingSampledImageUpdateAfterBind",
                +[](DeviceFeatures& f) -> VkBool32& {
                    return f.mVulkan12.descriptorBindingSampledImageUpdateAfterBind;
                } },
            RequiredFeature{ "shaderSampledImageArrayNonUniformIndexing",
                +[](DeviceFeatures& f) -> VkBool32& { return f.mVulkan12.shaderSampledImageArrayNonUniformIndexing; } },
            RequiredFeature{
                "scalarBlockLayout", +[](DeviceFeatures& f) -> VkBool32& { return f.mVulkan12.scalarBlockLayout; } },
            RequiredFeature{
                "timelineSemaphore", +[](DeviceFeatures& f) -> VkBool32& { return f.mVulkan12.timelineSemaphore; } },
            RequiredFeature{
                "hostQueryReset", +[](DeviceFeatures& f) -> VkBool32& { return f.mVulkan12.hostQueryReset; } },

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
            RequiredFeature{ "rayTracingPipeline",
                +[](DeviceFeatures& f) -> VkBool32& { return f.mRayTracingPipeline.rayTracingPipeline; } },
            RequiredFeature{ "rayTraversalPrimitiveCulling",
                +[](DeviceFeatures& f) -> VkBool32& { return f.mRayTracingPipeline.rayTraversalPrimitiveCulling; } },
            RequiredFeature{ "rayQuery", +[](DeviceFeatures& f) -> VkBool32& { return f.mRayQuery.rayQuery; } },
            RequiredFeature{ "rayTracingPositionFetch",
                +[](DeviceFeatures& f) -> VkBool32& { return f.mPositionFetch.rayTracingPositionFetch; } },
            RequiredFeature{ "rayTracingMaintenance1",
                +[](DeviceFeatures& f) -> VkBool32& { return f.mRayTracingMaintenance1.rayTracingMaintenance1; } },
            RequiredFeature{ "micromap", +[](DeviceFeatures& f) -> VkBool32& { return f.mOpacityMicromap.micromap; } },
            RequiredFeature{ "pipelineExecutableInfo",
                +[](DeviceFeatures& f) -> VkBool32& { return f.mPipelineExecutable.pipelineExecutableInfo; } },
            RequiredFeature{ "rayTracingInvocationReorder",
                +[](DeviceFeatures& f) -> VkBool32& { return f.mInvocationReorder.rayTracingInvocationReorder; } },
        };
    }

    DeviceFeatures::DeviceFeatures()
    {
        void* next = nullptr;
        chain(next, mInvocationReorder, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_FEATURES_EXT);
        chain(next, mOpacityMicromap, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_FEATURES_EXT);
        chain(next, mPipelineExecutable, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR);
        chain(next, mRayTracingMaintenance1, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_MAINTENANCE_1_FEATURES_KHR);
        chain(next, mPositionFetch, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_POSITION_FETCH_FEATURES_KHR);
        chain(next, mRayQuery, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR);
        chain(next, mRayTracingPipeline, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR);
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
        chain(next, mOpacityMicromap, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_PROPERTIES_EXT);
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
