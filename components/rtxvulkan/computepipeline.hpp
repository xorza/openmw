#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>

#include <vulkan/vulkan_core.h>

#include "owned.hpp"
#include "pipelinelayout.hpp"

namespace Rtx
{
    class Device;

    /// A compute pipeline, the descriptor set layout it is addressed through, and the pipeline
    /// layout that ties the two together.
    ///
    /// **The three are one object because they fail as one.** A constructor that throws gets no
    /// destructor, so a pass that made these itself had to unwind them by hand or leave a layout
    /// behind for `vkDestroyDevice` to find — which the layers report and the abort policy turns
    /// into an abort with no message. Held as members that end themselves, whatever finished being
    /// constructed is destroyed when the pass's own constructor throws, and there is no unwind to
    /// write.
    ///
    /// `TracePipeline` is the same object for a launch. Both address themselves through
    /// `PipelineLayout`.
    class ComputePipeline
    {
    public:
        /// Neither span outlives the call: both are read into Vulkan's own copies here, which is
        /// what lets a caller pass the address of one of its own parameters.
        ///
        /// @param pushConstantBytes the whole range, at offset zero, visible to the compute stage.
        /// @param laterSets layouts bound after set zero — the bindless texture array, where a pass
        ///        reads one. A pipeline layout has to name every set it will ever be handed.
        /// @param module the compiled SPIR-V the build wrote, by path.
        /// @param name what a capture calls the pipeline.
        /// @param specialization one word per specialization constant, `constant_id` `i` taking
        ///        `specialization[i]`. Words because that is what every constant this renderer
        ///        specializes on is — a `bool` reaches SPIR-V as a 32-bit value like a `uint` does —
        ///        and because the alternative is a caller building map entries for a table whose
        ///        offsets are its own indices.
        ComputePipeline(const Device& device, std::span<const VkDescriptorSetLayoutBinding> bindings,
            std::uint32_t pushConstantBytes, std::span<const VkDescriptorSetLayout> laterSets,
            const std::filesystem::path& module, std::string_view name,
            std::span<const std::uint32_t> specialization = {});

        ComputePipeline(const ComputePipeline&) = delete;
        ComputePipeline& operator=(const ComputePipeline&) = delete;

        VkPipeline getHandle() const { return mHandle.get(); }

        /// What descriptors are pushed against and push constants are written through.
        VkPipelineLayout getLayout() const { return mLayout.getHandle(); }

    private:
        PipelineLayout mLayout;
        Owned<VkPipeline, vkDestroyPipeline> mHandle;
    };
}
