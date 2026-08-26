#include "compositepass.hpp"

#include <array>
#include <cassert>

#include "commands.hpp"
#include "gbuffer.hpp"
#include "image.hpp"

namespace Rtx
{
    namespace
    {
        std::uint32_t groupsFor(std::uint32_t extent)
        {
            return (extent + Shaders::COMPOSITE_WORKGROUP - 1) / Shaders::COMPOSITE_WORKGROUP;
        }

        /// Three channels in, the running sum, and the frame out — all storage images, all pushed.
        constexpr std::array<VkDescriptorSetLayoutBinding, 5> sBindings{
            VkDescriptorSetLayoutBinding{ 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT },
            VkDescriptorSetLayoutBinding{ 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT },
            VkDescriptorSetLayoutBinding{ 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT },
            VkDescriptorSetLayoutBinding{ 3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT },
            VkDescriptorSetLayoutBinding{ 4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT },
        };
    }

    CompositePass::CompositePass(const Device& device, CommandPool& pool, const std::filesystem::path& shaderDirectory)
        : mPipeline(device, sBindings, sizeof(Shaders::CompositeConstants), {}, shaderDirectory / "composite.comp.spv",
              "composite")
        , mNoHistory(device, 1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT, "no-history")
    {
        // A bound storage image has to be in the layout its descriptor names whether the shader
        // reads it or not, so the one texel is laid out once and then left alone forever.
        pool.submitAndWait([this](VkCommandBuffer commands) {
            mNoHistory.transition(commands, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        });
    }

    void CompositePass::record(VkCommandBuffer commands, const GBuffer& buffer, const Image& indirect,
        const Image* history, const Image& colour, const Shaders::CompositeConstants& constants) const
    {
        assert(buffer.getWidth() >= constants.mWidth && buffer.getHeight() >= constants.mHeight);
        assert(indirect.getWidth() >= constants.mWidth && indirect.getHeight() >= constants.mHeight);
        assert(colour.getWidth() >= constants.mWidth && colour.getHeight() >= constants.mHeight);

        // A sum has to cover the frame it is a sum of; a stand-in never read does not.
        assert(constants.mAccumulate == 0 || history != nullptr);
        assert(history == nullptr
            || (history->getWidth() >= constants.mWidth && history->getHeight() >= constants.mHeight));

        const Image& sum = history != nullptr ? *history : mNoHistory;

        const std::array<VkDescriptorImageInfo, 5> images{
            VkDescriptorImageInfo{ VK_NULL_HANDLE, buffer.getDirect().getView(), VK_IMAGE_LAYOUT_GENERAL },
            VkDescriptorImageInfo{ VK_NULL_HANDLE, indirect.getView(), VK_IMAGE_LAYOUT_GENERAL },
            VkDescriptorImageInfo{ VK_NULL_HANDLE, buffer.getAlbedo().getView(), VK_IMAGE_LAYOUT_GENERAL },
            VkDescriptorImageInfo{ VK_NULL_HANDLE, sum.getView(), VK_IMAGE_LAYOUT_GENERAL },
            VkDescriptorImageInfo{ VK_NULL_HANDLE, colour.getView(), VK_IMAGE_LAYOUT_GENERAL },
        };

        std::array<VkWriteDescriptorSet, 5> writes{};
        for (std::uint32_t i = 0; i < images.size(); ++i)
            writes[i] = VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstBinding = i,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .pImageInfo = &images[i],
            };

        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, mPipeline.getHandle());
        vkCmdPushDescriptorSet(commands, VK_PIPELINE_BIND_POINT_COMPUTE, mPipeline.getLayout(), 0,
            static_cast<std::uint32_t>(writes.size()), writes.data());
        vkCmdPushConstants(
            commands, mPipeline.getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
        vkCmdDispatch(commands, groupsFor(constants.mWidth), groupsFor(constants.mHeight), 1);
    }
}
