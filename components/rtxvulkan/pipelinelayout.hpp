#pragma once

#include <cstdint>
#include <span>

#include <vulkan/vulkan_core.h>

#include "owned.hpp"

namespace Rtx
{
    class Device;

    /// A pass's own descriptor set layout and the pipeline layout that names it and the sets bound
    /// after it.
    ///
    /// **One statement of it, because a compute pipeline and a trace pipeline are addressed the same
    /// way.** The two differ in how a shader is compiled and how work is launched, and in nothing
    /// about how descriptors reach it — `VisibilityPass` hands both the same bindings and the same
    /// three later sets. Written twice, the two would drift the moment a flag changed.
    ///
    /// Set zero is always a push descriptor set: nothing in this renderer wants a descriptor pool on
    /// the frame path.
    class PipelineLayout
    {
    public:
        /// Neither span outlives the call: both are read into Vulkan's own copies here, which is
        /// what lets a caller pass the address of one of its own parameters.
        ///
        /// @param bindings set zero, with each binding naming the stages that read it.
        /// @param pushConstantBytes the whole range, at offset zero. A range of no bytes is not one
        ///        Vulkan will take, and a pass whose constants outgrew the push limit and moved into
        ///        a buffer asks for exactly that — so none is declared.
        /// @param pushStages which stage the push constants are visible to.
        /// @param laterSets layouts bound after set zero. A pipeline layout has to name every set it
        ///        will ever be handed.
        PipelineLayout(const Device& device, std::span<const VkDescriptorSetLayoutBinding> bindings,
            std::uint32_t pushConstantBytes, VkShaderStageFlags pushStages,
            std::span<const VkDescriptorSetLayout> laterSets);

        VkDescriptorSetLayout getSetLayout() const { return mSetLayout.get(); }

        /// What descriptors are pushed against and push constants are written through.
        VkPipelineLayout getHandle() const { return mHandle.get(); }

    private:
        Owned<VkDescriptorSetLayout, vkDestroyDescriptorSetLayout> mSetLayout;
        Owned<VkPipelineLayout, vkDestroyPipelineLayout> mHandle;
    };
}
