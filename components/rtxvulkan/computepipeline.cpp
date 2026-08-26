#include "computepipeline.hpp"

#include "device.hpp"
#include "result.hpp"
#include "shadermodule.hpp"

namespace Rtx
{
    ComputePipeline::ComputePipeline(const Device& device, std::span<const VkDescriptorSetLayoutBinding> bindings,
        std::uint32_t pushConstantBytes, std::span<const VkDescriptorSetLayout> laterSets,
        const std::filesystem::path& module, std::string_view name, std::span<const std::uint32_t> specialization)
        : mDevice(device)
    {
        // The renderer's only hand-written unwind, and the reason this type exists: three handles
        // are made in sequence and any of the three can fail, so what the earlier ones took has to
        // be given back before the failure leaves this constructor.
        try
        {
            const VkDescriptorSetLayoutCreateInfo layout{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT,
                .bindingCount = static_cast<std::uint32_t>(bindings.size()),
                .pBindings = bindings.data(),
            };
            checkVk(vkCreateDescriptorSetLayout(mDevice.getHandle(), &layout, nullptr, &mSetLayout),
                "vkCreateDescriptorSetLayout");

            // Set zero is this pipeline's own; whatever the caller named follows it, in order.
            std::vector<VkDescriptorSetLayout> sets;
            sets.reserve(laterSets.size() + 1);
            sets.push_back(mSetLayout);
            sets.insert(sets.end(), laterSets.begin(), laterSets.end());

            const VkPushConstantRange range{
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                .size = pushConstantBytes,
            };
            // A range of no bytes is not a range Vulkan will take, and a pass whose constants
            // outgrew the push limit and moved into a buffer asks for exactly that.
            const bool pushes = pushConstantBytes > 0;
            const VkPipelineLayoutCreateInfo pipelineLayout{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                .setLayoutCount = static_cast<std::uint32_t>(sets.size()),
                .pSetLayouts = sets.data(),
                .pushConstantRangeCount = pushes ? 1u : 0u,
                .pPushConstantRanges = pushes ? &range : nullptr,
            };
            checkVk(vkCreatePipelineLayout(mDevice.getHandle(), &pipelineLayout, nullptr, &mLayout),
                "vkCreatePipelineLayout");

            const ShaderModule compiled(mDevice, module);
            // One entry per word, at its own index's offset. Built here rather than by the caller
            // because it is the same table every time and its contents are the caller's indices.
            std::vector<VkSpecializationMapEntry> entries(specialization.size());
            for (std::uint32_t at = 0; at < entries.size(); ++at)
                entries[at] = VkSpecializationMapEntry{ at, at * static_cast<std::uint32_t>(sizeof(std::uint32_t)),
                    sizeof(std::uint32_t) };

            const VkSpecializationInfo constants{
                .mapEntryCount = static_cast<std::uint32_t>(entries.size()),
                .pMapEntries = entries.data(),
                .dataSize = specialization.size_bytes(),
                .pData = specialization.data(),
            };

            const VkComputePipelineCreateInfo pipeline{
                .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                // **Asked for at creation, because it cannot be asked for afterwards.** The cost is
                // to compiling the pipeline and not to running it, and every pipeline here is made
                // once — where the answer it buys is the only way to see a register count, which is
                // what an occupancy figure is made of.
                .flags = VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR,
                .stage = {
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                    .module = compiled.getHandle(),
                    .pName = "main",
                    .pSpecializationInfo = specialization.empty() ? nullptr : &constants,
                },
                .layout = mLayout,
            };
            checkVk(vkCreateComputePipelines(
                        mDevice.getHandle(), mDevice.getPipelineCache(), 1, &pipeline, nullptr, &mHandle),
                "vkCreateComputePipelines");

            mDevice.setName(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<std::uint64_t>(mHandle), name);
            mDevice.reportPipeline(mHandle, name);
        }
        catch (...)
        {
            destroy();
            throw;
        }
    }

    ComputePipeline::~ComputePipeline()
    {
        destroy();
    }

    void ComputePipeline::destroy()
    {
        if (mHandle != VK_NULL_HANDLE)
            vkDestroyPipeline(mDevice.getHandle(), mHandle, nullptr);
        if (mLayout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(mDevice.getHandle(), mLayout, nullptr);
        if (mSetLayout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(mDevice.getHandle(), mSetLayout, nullptr);
    }
}
