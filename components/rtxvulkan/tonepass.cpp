#include "tonepass.hpp"

#include <array>
#include <cassert>

#include "image.hpp"

namespace Rtx
{
    namespace
    {
        std::uint32_t groupsFor(std::uint32_t extent)
        {
            return (extent + Shaders::TONE_WORKGROUP - 1) / Shaders::TONE_WORKGROUP;
        }

        /// The frame in, the picture out, and the one float between them. All pushed.
        constexpr std::array<VkDescriptorSetLayoutBinding, 3> sBindings{
            VkDescriptorSetLayoutBinding{ 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT },
            VkDescriptorSetLayoutBinding{ 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT },
            VkDescriptorSetLayoutBinding{ 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT },
        };
    }

    TonePass::TonePass(const Device& device, const std::filesystem::path& shaderDirectory)
        : mPipeline(device, sBindings, sizeof(Shaders::ToneConstants), {}, shaderDirectory / "tone.comp.spv", "tone")
    {
    }

    void TonePass::record(VkCommandBuffer commands, const Image& colour, VkBuffer exposure, const Image& target,
        std::uint32_t width, std::uint32_t height) const
    {
        assert(width <= target.getWidth() && height <= target.getHeight());

        const std::array<VkDescriptorImageInfo, 2> images{
            VkDescriptorImageInfo{ VK_NULL_HANDLE, colour.getView(), VK_IMAGE_LAYOUT_GENERAL },
            VkDescriptorImageInfo{ VK_NULL_HANDLE, target.getView(), VK_IMAGE_LAYOUT_GENERAL },
        };
        const VkDescriptorBufferInfo scale{ exposure, 0, VK_WHOLE_SIZE };

        std::array<VkWriteDescriptorSet, 3> writes{};
        for (std::uint32_t i = 0; i < images.size(); ++i)
            writes[i] = VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstBinding = i,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .pImageInfo = &images[i],
            };

        writes[2] = VkWriteDescriptorSet{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstBinding = 2,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &scale,
        };

        const Shaders::ToneConstants constants{
            .mWidth = width,
            .mHeight = height,
        };

        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, mPipeline.getHandle());
        vkCmdPushDescriptorSet(commands, VK_PIPELINE_BIND_POINT_COMPUTE, mPipeline.getLayout(), 0,
            static_cast<std::uint32_t>(writes.size()), writes.data());
        vkCmdPushConstants(
            commands, mPipeline.getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
        vkCmdDispatch(commands, groupsFor(constants.mWidth), groupsFor(constants.mHeight), 1);
    }
}
