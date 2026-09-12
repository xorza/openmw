#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>

#include <vulkan/vulkan_core.h>

#include "handles.hpp"
#include "owned.hpp"

namespace Rtx
{
    class Device;

    /// A compute pipeline, the descriptor set layout it is addressed through, and the pipeline
    /// layout that ties the two together — one object because they fail as one: a constructor
    /// that throws gets no destructor, so a pass that made these itself left a layout behind for
    /// `vkDestroyDevice` to find. `TracePipeline` is the same object for a launch.
    class ComputePipeline
    {
    public:
        /// Neither span outlives the call.
        ///
        /// @param pushConstantBytes the whole range, at offset zero, visible to the compute stage.
        /// @param laterSets layouts bound after set zero. A pipeline layout has to name every set
        ///        it will ever be handed.
        /// @param module the compiled SPIR-V the build wrote, by path.
        /// @param name what a capture calls the pipeline.
        /// @param specialization one word per specialization constant, `constant_id` `i` taking
        ///        `specialization[i]` — a `bool` reaches SPIR-V as a 32-bit value like a `uint`.
        ComputePipeline(const Device& device, std::span<const VkDescriptorSetLayoutBinding> bindings,
            std::uint32_t pushConstantBytes, std::span<const VkDescriptorSetLayout> laterSets,
            const std::filesystem::path& module, std::string_view name,
            std::span<const std::uint32_t> specialization = {});

        VkPipeline getHandle() const { return mHandle.get(); }

        /// What descriptors are pushed against and push constants are written through.
        VkPipelineLayout getLayout() const { return mLayout.getHandle(); }

    private:
        PipelineLayout mLayout;
        Owned<VkPipeline, vkDestroyPipeline> mHandle;
    };
}
