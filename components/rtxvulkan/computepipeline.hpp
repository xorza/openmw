#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "handles.hpp"
#include "owned.hpp"

namespace Rtx
{
    class Device;

    /// The map entries a table of specialization words needs, and the `VkSpecializationInfo` over
    /// them. Built here rather than by the caller, because it is the same table every time and
    /// its contents are the caller's own indices: `constant_id` `i` takes word `i`, at word `i`'s
    /// offset. Words because that is what every constant this renderer specializes on is — a `bool`
    /// reaches SPIR-V as a 32-bit value like a `uint` does. Does not outlive the words it was made
    /// from, and neither does what `getInfo` points at.
    class Specialization
    {
    public:
        explicit Specialization(std::span<const std::uint32_t> words)
            : mEntries(words.size())
        {
            for (std::uint32_t at = 0; at < mEntries.size(); ++at)
                mEntries[at] = VkSpecializationMapEntry{ at, at * static_cast<std::uint32_t>(sizeof(std::uint32_t)),
                    sizeof(std::uint32_t) };

            mInfo = VkSpecializationInfo{
                .mapEntryCount = static_cast<std::uint32_t>(mEntries.size()),
                .pMapEntries = mEntries.data(),
                .dataSize = words.size_bytes(),
                .pData = words.data(),
            };
        }

        Specialization(const Specialization&) = delete;
        Specialization& operator=(const Specialization&) = delete;

        /// What a stage's `pSpecializationInfo` takes, or null where nothing was specialized.
        const VkSpecializationInfo* getInfo() const { return mEntries.empty() ? nullptr : &mInfo; }

    private:
        std::vector<VkSpecializationMapEntry> mEntries;
        VkSpecializationInfo mInfo{};
    };

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
