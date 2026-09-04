#include "micromappass.hpp"

#include <cstdint>
#include <span>

#include "device.hpp"

namespace Rtx
{
    namespace
    {
        std::uint32_t groupsFor(std::uint32_t triangles)
        {
            return (triangles + Shaders::MICROMAP_WORKGROUP - 1) / Shaders::MICROMAP_WORKGROUP;
        }
    }

    MicromapPass::MicromapPass(
        const Device& device, VkDescriptorSetLayout textureLayout, const std::filesystem::path& shaderDirectory)
        : mPipeline(device, {}, sizeof(Shaders::MicromapConstants), std::span(&textureLayout, 1),
            shaderDirectory / "micromap.comp.spv", "micromap")
    {
    }

    void MicromapPass::begin(VkCommandBuffer commands, VkDescriptorSet textures) const
    {
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, mPipeline.getHandle());
        vkCmdBindDescriptorSets(
            commands, VK_PIPELINE_BIND_POINT_COMPUTE, mPipeline.getLayout(), 1, 1, &textures, 0, nullptr);
    }

    void MicromapPass::bake(VkCommandBuffer commands, const Shaders::MicromapConstants& constants) const
    {
        vkCmdPushConstants(
            commands, mPipeline.getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
        vkCmdDispatch(commands, groupsFor(constants.mCount), 1, 1);
    }
}
