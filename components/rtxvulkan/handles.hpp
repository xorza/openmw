#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>

#include <vulkan/vulkan_core.h>

#include "owned.hpp"

namespace Rtx
{
    class Device;

    /// The handles this renderer makes in one place and holds in many, each as its `Owned`: the
    /// type already says what it owns and when it ends, so what is left to say is how one is made.
    using ShaderModule = Owned<VkShaderModule, vkDestroyShaderModule>;
    using SetLayout = Owned<VkDescriptorSetLayout, vkDestroyDescriptorSetLayout>;
    using Sampler = Owned<VkSampler, vkDestroySampler>;

    /// A `VkShaderModule` built from a SPIR-V file the build produced and validated; what this
    /// checks is that the file is the one the build wrote, because a truncated `.spv` is otherwise
    /// a driver crash with no explanation.
    ShaderModule loadShaderModule(const Device& device, const std::filesystem::path& path);

    /// A descriptor set layout, for `GBuffer::describeLayout` and its siblings to build theirs
    /// through. `flags` is what a push descriptor set needs; `next` is binding flags for a bindless
    /// set, read here and never kept.
    SetLayout makeSetLayout(const Device& device, std::span<const VkDescriptorSetLayoutBinding> bindings,
        VkDescriptorSetLayoutCreateFlags flags = 0, const void* next = nullptr);

    /// The two shapes this renderer reads images through, both linear over the whole chain, because
    /// written out per class the fields drift where nothing decides. Targets clamp to the edge,
    /// because a bloom level or a volume slice runs to the edge of what it was given; content
    /// repeats, because Morrowind's textures tile.
    Sampler makeTargetSampler(const Device& device, std::string_view name);
    Sampler makeContentSampler(const Device& device, std::string_view name);

    /// A pass's own descriptor set layout and the pipeline layout that names it and the sets bound
    /// after it — one statement for compute, trace and graphics pipelines, which differ in nothing
    /// about how descriptors reach them. Set zero is always a push descriptor set: nothing in this
    /// renderer wants a descriptor pool on the frame path.
    class PipelineLayout
    {
    public:
        /// Neither span outlives the call. `bindings` is set zero; `pushConstantBytes` is the whole
        /// range at offset zero, and nought declares none, because Vulkan takes no empty range and a
        /// pass whose constants moved into a buffer asks for exactly that; `laterSets` is every set
        /// the layout will ever be handed after set zero.
        PipelineLayout(const Device& device, std::span<const VkDescriptorSetLayoutBinding> bindings,
            std::uint32_t pushConstantBytes, VkShaderStageFlags pushStages,
            std::span<const VkDescriptorSetLayout> laterSets);

        VkPipelineLayout getHandle() const { return mHandle.get(); }

    private:
        SetLayout mSetLayout;
        Owned<VkPipelineLayout, vkDestroyPipelineLayout> mHandle;
    };
}
