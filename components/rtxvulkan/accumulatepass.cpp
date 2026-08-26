#include "accumulatepass.hpp"

#include <array>
#include <cassert>

#include <components/rtx/shaders/gbuffer.h>

#include "gbuffer.hpp"

namespace Rtx
{
    namespace
    {
        std::uint32_t groupsFor(std::uint32_t extent)
        {
            return (extent + Shaders::ACCUMULATE_WORKGROUP - 1) / Shaders::ACCUMULATE_WORKGROUP;
        }

        /// The channel being blended, the four the frame describes it with, and the three of each
        /// that carry a history across. All storage images, all pushed.
        constexpr std::size_t sBindingCount = 11;

        /// Eleven of one kind, so a loop rather than eleven lines — `AtrousPass` spells its four out
        /// because four is not yet a list.
        constexpr std::array<VkDescriptorSetLayoutBinding, sBindingCount> describeBindings()
        {
            std::array<VkDescriptorSetLayoutBinding, sBindingCount> bindings{};
            for (std::uint32_t i = 0; i < bindings.size(); ++i)
                bindings[i] = VkDescriptorSetLayoutBinding{ i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                    VK_SHADER_STAGE_COMPUTE_BIT, nullptr };

            return bindings;
        }

        constexpr std::array<VkDescriptorSetLayoutBinding, sBindingCount> sBindings = describeBindings();
    }

    AccumulatePass::AccumulatePass(const Device& device, const std::filesystem::path& shaderDirectory)
        : mDevice(device)
        , mPipeline(device, sBindings, sizeof(Shaders::AccumulateConstants), {},
              shaderDirectory / "accumulate.comp.spv", "accumulate")
    {
    }

    void AccumulatePass::resize(std::uint32_t width, std::uint32_t height)
    {
        if (mColour[0] != nullptr && mColour[0]->getWidth() == width && mColour[0]->getHeight() == height)
            return;

        for (std::size_t i = 0; i < 2; ++i)
        {
            mColour[i] = std::make_unique<Image>(mDevice, width, height, GBUFFER_RADIANCE, VK_IMAGE_USAGE_STORAGE_BIT,
                i == 0 ? "accumulate-colour-0" : "accumulate-colour-1");
            mSurface[i] = std::make_unique<Image>(mDevice, width, height, GBUFFER_GUIDE, VK_IMAGE_USAGE_STORAGE_BIT,
                i == 0 ? "accumulate-surface-0" : "accumulate-surface-1");
            mMoments[i] = std::make_unique<Image>(mDevice, width, height, GBUFFER_RADIANCE, VK_IMAGE_USAGE_STORAGE_BIT,
                i == 0 ? "accumulate-moments-0" : "accumulate-moments-1");
        }

        mCurrent = 0;
        mFresh = true;
    }

    const Image& AccumulatePass::record(
        VkCommandBuffer commands, const GBuffer& buffer, const Shaders::Camera& camera, bool reset)
    {
        assert(mColour[0] != nullptr && "record before resize");
        assert(mColour[0]->getWidth() >= camera.mWidth && mColour[0]->getHeight() >= camera.mHeight);

        const std::size_t previous = mCurrent;
        mCurrent = 1 - mCurrent;

        // **The first frame after a resize has nothing behind it**, and an image whose contents were
        // never written is not zero — it is whatever the allocation held. Discarding it is what makes
        // the reset below a statement about the history rather than about the memory.
        const VkImageLayout held = mFresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
        for (const Image* image : { mColour[previous].get(), mSurface[previous].get(), mMoments[previous].get() })
            image->transition(commands, held, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

        for (const Image* image : { mColour[mCurrent].get(), mSurface[mCurrent].get(), mMoments[mCurrent].get() })
            image->transition(commands, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

        const std::array<VkDescriptorImageInfo, sBindingCount> images{
            VkDescriptorImageInfo{ VK_NULL_HANDLE, buffer.getIndirect().getView(), VK_IMAGE_LAYOUT_GENERAL },
            VkDescriptorImageInfo{ VK_NULL_HANDLE, buffer.getMotion().getView(), VK_IMAGE_LAYOUT_GENERAL },
            VkDescriptorImageInfo{ VK_NULL_HANDLE, buffer.getGuide().getView(), VK_IMAGE_LAYOUT_GENERAL },
            VkDescriptorImageInfo{ VK_NULL_HANDLE, buffer.getDepth().getView(), VK_IMAGE_LAYOUT_GENERAL },
            VkDescriptorImageInfo{ VK_NULL_HANDLE, buffer.getBiasMask().getView(), VK_IMAGE_LAYOUT_GENERAL },
            VkDescriptorImageInfo{ VK_NULL_HANDLE, mColour[previous]->getView(), VK_IMAGE_LAYOUT_GENERAL },
            VkDescriptorImageInfo{ VK_NULL_HANDLE, mSurface[previous]->getView(), VK_IMAGE_LAYOUT_GENERAL },
            VkDescriptorImageInfo{ VK_NULL_HANDLE, mMoments[previous]->getView(), VK_IMAGE_LAYOUT_GENERAL },
            VkDescriptorImageInfo{ VK_NULL_HANDLE, mColour[mCurrent]->getView(), VK_IMAGE_LAYOUT_GENERAL },
            VkDescriptorImageInfo{ VK_NULL_HANDLE, mSurface[mCurrent]->getView(), VK_IMAGE_LAYOUT_GENERAL },
            VkDescriptorImageInfo{ VK_NULL_HANDLE, mMoments[mCurrent]->getView(), VK_IMAGE_LAYOUT_GENERAL },
        };

        std::array<VkWriteDescriptorSet, sBindingCount> writes{};
        for (std::uint32_t i = 0; i < images.size(); ++i)
            writes[i] = VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstBinding = i,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .pImageInfo = &images[i],
            };

        const Shaders::AccumulateConstants constants{
            .mCamera = camera,
            .mReset = (reset || mFresh) ? 1u : 0u,
        };

        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, mPipeline.getHandle());
        vkCmdPushDescriptorSet(commands, VK_PIPELINE_BIND_POINT_COMPUTE, mPipeline.getLayout(), 0,
            static_cast<std::uint32_t>(writes.size()), writes.data());
        vkCmdPushConstants(
            commands, mPipeline.getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
        vkCmdDispatch(commands, groupsFor(camera.mWidth), groupsFor(camera.mHeight), 1);

        mFresh = false;

        return *mMoments[mCurrent];
    }
}
