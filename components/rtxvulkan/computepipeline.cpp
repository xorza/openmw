#include "computepipeline.hpp"

#include <vector>

#include "device.hpp"
#include "result.hpp"
#include "shadermodule.hpp"
#include "specialization.hpp"

namespace Rtx
{
    ComputePipeline::ComputePipeline(const Device& device, std::span<const VkDescriptorSetLayoutBinding> bindings,
        std::uint32_t pushConstantBytes, std::span<const VkDescriptorSetLayout> laterSets,
        const std::filesystem::path& module, std::string_view name, std::span<const std::uint32_t> specialization)
        : mLayout(device, bindings, pushConstantBytes, VK_SHADER_STAGE_COMPUTE_BIT, laterSets)
    {
        const ShaderModule compiled(device, module);
        const Specialization constants(specialization);

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
                .pSpecializationInfo = constants.getInfo(),
            },
            .layout = mLayout.getHandle(),
        };
        checkVk(vkCreateComputePipelines(device.getHandle(), device.getPipelineCache(), 1, &pipeline, nullptr,
                    mHandle.put(device.getHandle())),
            "vkCreateComputePipelines");

        device.setName(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<std::uint64_t>(mHandle.get()), name);
        device.reportPipeline(mHandle.get(), name);
    }
}
