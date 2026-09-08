#include "compositepass.hpp"

#include <array>
#include <cassert>
#include <cstdint>

#include "commands.hpp"
#include "dispatch.hpp"
#include "gbuffer.hpp"
#include "image.hpp"

namespace Rtx
{
    namespace
    {
        /// Three channels in, the running sum, and the frame out — all storage images, all pushed.
        constexpr std::array<VkDescriptorSetLayoutBinding, 5> sBindings
            = computeBindings<5>(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    }

    CompositePass::CompositePass(const Device& device, CommandPool& pool, const std::filesystem::path& shaderDirectory)
        : mPipeline(device, sBindings, sizeof(Shaders::CompositeConstants), {}, shaderDirectory / "composite.comp.spv",
            "composite")
        , mNoSum(device, 1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT, "no-sum")
    {
        // A bound storage image has to be in the layout its descriptor names whether the shader
        // reads it or not, so the one texel is laid out here and never leaves `GENERAL` again.
        pool.submitAndWait([this](VkCommandBuffer commands) {
            mNoSum.transition(commands, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        });
    }

    void CompositePass::record(VkCommandBuffer commands, const GBuffer& buffer, const Image& indirect, const Image* sum,
        const Image& colour, const Shaders::CompositeConstants& constants) const
    {
        assert(buffer.getWidth() >= constants.mWidth && buffer.getHeight() >= constants.mHeight);
        assert(indirect.getWidth() >= constants.mWidth && indirect.getHeight() >= constants.mHeight);
        assert(colour.getWidth() >= constants.mWidth && colour.getHeight() >= constants.mHeight);

        // A sum has to cover the frame it is a sum of; a stand-in never read does not.
        assert(constants.mAccumulate == 0 || sum != nullptr);
        assert(sum == nullptr || (sum->getWidth() >= constants.mWidth && sum->getHeight() >= constants.mHeight));

        const Image& bound = sum != nullptr ? *sum : mNoSum;

        // **The stand-in is ordered here, and the real sum is the caller's to order.** Nothing
        // writes this one texel — the shader's store sits behind `mAccumulate`, and a stand-in is
        // bound only where that is nought — but synchronization validation reasons from the
        // descriptor set rather than from the branch, so two frames' composites read to it as two
        // unordered writes. One barrier on one texel is cheaper than a check nobody can leave on.
        if (sum == nullptr)
            mNoSum.transition(commands, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

        const std::array<VkDescriptorImageInfo, 5> images{
            VkDescriptorImageInfo{ VK_NULL_HANDLE, buffer.getDirect().getView(), VK_IMAGE_LAYOUT_GENERAL },
            VkDescriptorImageInfo{ VK_NULL_HANDLE, indirect.getView(), VK_IMAGE_LAYOUT_GENERAL },
            VkDescriptorImageInfo{ VK_NULL_HANDLE, buffer.getAlbedo().getView(), VK_IMAGE_LAYOUT_GENERAL },
            VkDescriptorImageInfo{ VK_NULL_HANDLE, bound.getView(), VK_IMAGE_LAYOUT_GENERAL },
            VkDescriptorImageInfo{ VK_NULL_HANDLE, colour.getView(), VK_IMAGE_LAYOUT_GENERAL },
        };

        const std::array<VkWriteDescriptorSet, 5> writes = storageImageWrites(images);

        dispatch(commands, mPipeline, writes, constants, groupsFor(constants.mWidth, Shaders::COMPOSITE_WORKGROUP),
            groupsFor(constants.mHeight, Shaders::COMPOSITE_WORKGROUP));
    }
}
