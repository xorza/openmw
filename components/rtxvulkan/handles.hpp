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

    /// A `VkShaderModule` built from a SPIR-V file the build produced.
    ///
    /// The build compiles every shader with `glslc` and runs `spirv-val` over the result, so a
    /// module that reaches here has already been validated. What this checks is that the *file* is
    /// the one the build wrote — a stale or truncated `.spv` is otherwise a driver crash with no
    /// explanation.
    ShaderModule loadShaderModule(const Device& device, const std::filesystem::path& path);

    /// A descriptor set layout. What each layout *is* stays with the thing that knows —
    /// `GBuffer::describeLayout` and its siblings build the binding list and hand back one of these.
    ///
    /// @param flags what a push descriptor set needs and a bound one does not.
    /// @param next binding flags, where the set is bindless. Read here and never kept.
    SetLayout makeSetLayout(const Device& device, std::span<const VkDescriptorSetLayoutBinding> bindings,
        VkDescriptorSetLayoutCreateFlags flags = 0, const void* next = nullptr);

    /// The two shapes this renderer reads images through, and every sampler in it is one of them.
    ///
    /// All of them are linear. What differs is whether an image tiles and whether it carries a
    /// chain, and those go together: a tiling image is content and content is mipped, while a
    /// pass's own target is neither. Written out per class, the pair drifts where nothing decides:
    /// a `maxLod` left at nought that reaches nothing only because the images below it happen to
    /// have one level, a border colour set on a sampler that clamps to the edge and so never reads
    /// one.
    ///
    /// `makeTargetSampler` is clamped to the edge, over the whole chain: what a pass reads its own
    /// targets through — a bloom level, a volume slice, a frame — because such a target runs to the
    /// edge of what it was given, and wrapping would fetch the far side of it.
    /// `makeContentSampler` repeats, over the whole chain, because Morrowind's textures tile and a
    /// great many of them rely on it.
    Sampler makeTargetSampler(const Device& device, std::string_view name);
    Sampler makeContentSampler(const Device& device, std::string_view name);

    /// A pass's own descriptor set layout and the pipeline layout that names it and the sets bound
    /// after it.
    ///
    /// **One statement of it, because every pipeline in this renderer is addressed the same way.**
    /// A compute pipeline, a trace pipeline and a graphics pipeline differ in how a shader is
    /// compiled and how work is launched, and in nothing about how descriptors reach it —
    /// `VisibilityPass` hands the first two the same bindings and the same three later sets. Written
    /// out per pipeline, the copies drift the moment a flag changes.
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

        /// What descriptors are pushed against and push constants are written through.
        VkPipelineLayout getHandle() const { return mHandle.get(); }

    private:
        SetLayout mSetLayout;
        Owned<VkPipelineLayout, vkDestroyPipelineLayout> mHandle;
    };
}
