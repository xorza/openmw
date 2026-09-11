#include "pipelinelayout.hpp"

#include <vector>

#include "device.hpp"
#include "result.hpp"

namespace Rtx
{
    PipelineLayout::PipelineLayout(const Device& device, std::span<const VkDescriptorSetLayoutBinding> bindings,
        std::uint32_t pushConstantBytes, VkShaderStageFlags pushStages,
        std::span<const VkDescriptorSetLayout> laterSets)
        : mSetLayout(device, bindings, VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT)
    {
        // Set zero is this pipeline's own; whatever the caller named follows it, in order.
        std::vector<VkDescriptorSetLayout> sets;
        sets.reserve(laterSets.size() + 1);
        sets.push_back(mSetLayout.getHandle());
        sets.insert(sets.end(), laterSets.begin(), laterSets.end());

        const VkPushConstantRange range{
            .stageFlags = pushStages,
            .size = pushConstantBytes,
        };
        const bool pushes = pushConstantBytes > 0;
        const VkPipelineLayoutCreateInfo pipelineLayout{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = static_cast<std::uint32_t>(sets.size()),
            .pSetLayouts = sets.data(),
            .pushConstantRangeCount = pushes ? 1u : 0u,
            .pPushConstantRanges = pushes ? &range : nullptr,
        };
        checkVk(vkCreatePipelineLayout(device.getHandle(), &pipelineLayout, nullptr, mHandle.put(device.getHandle())),
            "vkCreatePipelineLayout");
    }
}
