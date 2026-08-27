#include "bloompass.hpp"

#include <array>
#include <cassert>
#include <format>

#include <components/rtx/shaders/bloom.h>

#include "device.hpp"
#include "result.hpp"

namespace Rtx
{
    namespace
    {
        std::uint32_t groupsFor(std::uint32_t extent)
        {
            return (extent + Shaders::BLOOM_WORKGROUP - 1) / Shaders::BLOOM_WORKGROUP;
        }

        /// What is being read, and what is being written. The first is sampled rather than loaded,
        /// because both kernels are counted in bilinear fetches.
        constexpr std::array<VkDescriptorSetLayoutBinding, 2> sBindings{
            VkDescriptorSetLayoutBinding{
                0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT },
            VkDescriptorSetLayoutBinding{ 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT },
        };
    }

    BloomPass::BloomPass(const Device& device, const std::filesystem::path& shaderDirectory)
        : mDevice(device)
        , mHalvePipeline(device, sBindings, sizeof(Shaders::BloomConstants), {}, shaderDirectory / "bloomdown.comp.spv",
              "bloom halve")
        , mSpreadPipeline(device, sBindings, sizeof(Shaders::BloomConstants), {}, shaderDirectory / "bloomup.comp.spv",
              "bloom spread")
    {
        // After the pipelines, not before: a member that throws while being constructed leaves the
        // ones already built to their own destructors, and a handle made in this body would have
        // none. Nothing after this can throw.
        const VkSamplerCreateInfo sampler{
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_LINEAR,
            .minFilter = VK_FILTER_LINEAR,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
        };
        checkVk(vkCreateSampler(mDevice.getHandle(), &sampler, nullptr, &mSampler), "vkCreateSampler");
    }

    BloomPass::~BloomPass()
    {
        if (mSampler != VK_NULL_HANDLE)
            vkDestroySampler(mDevice.getHandle(), mSampler, nullptr);
    }

    void BloomPass::resize(std::uint32_t width, std::uint32_t height)
    {
        if (!mLevels.empty() && mLevels.front()->getWidth() == width / 2 && mLevels.front()->getHeight() == height / 2)
            return;

        mLevels.clear();

        for (std::uint32_t level = 0; level < Shaders::BLOOM_LEVELS; ++level)
        {
            width /= 2;
            height /= 2;

            if (width < Shaders::BLOOM_NARROWEST || height < Shaders::BLOOM_NARROWEST)
                break;

            // `TRANSFER_SRC` because the levels are the whole of what this pass produces and so the
            // only thing a reader can check it by — `GBuffer::sReadable` carries the bit for the
            // same reason, and it costs no memory either.
            mLevels.push_back(std::make_unique<Image>(mDevice, width, height, BLOOM_LEVEL,
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                std::format("bloom level {}", level)));
        }
    }

    void BloomPass::handOver(VkCommandBuffer commands, const Image& level) const
    {
        level.transition(commands, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }

    void BloomPass::run(VkCommandBuffer commands, const ComputePipeline& pipeline, const Image& source,
        const Image& target, float mix) const
    {
        // **Sampled from `GENERAL` rather than moved to a read-only layout.** A level is written as
        // a storage image and read as a sampled one within a few dispatches of each other, and the
        // layout this renderer keeps everything in is one both accesses are legal from.
        const std::array<VkDescriptorImageInfo, 2> images{
            VkDescriptorImageInfo{ mSampler, source.getView(), VK_IMAGE_LAYOUT_GENERAL },
            VkDescriptorImageInfo{ VK_NULL_HANDLE, target.getView(), VK_IMAGE_LAYOUT_GENERAL },
        };

        std::array<VkWriteDescriptorSet, 2> writes{};
        for (std::uint32_t binding = 0; binding < images.size(); ++binding)
            writes[binding] = VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstBinding = binding,
                .descriptorCount = 1,
                .descriptorType = sBindings[binding].descriptorType,
                .pImageInfo = &images[binding],
            };

        const Shaders::BloomConstants constants{
            .mWidth = target.getWidth(),
            .mHeight = target.getHeight(),
            .mTexel
            = osg::Vec2f(1.0f / static_cast<float>(source.getWidth()), 1.0f / static_cast<float>(source.getHeight())),
            .mMix = mix,
        };

        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.getHandle());
        vkCmdPushDescriptorSet(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.getLayout(), 0,
            static_cast<std::uint32_t>(writes.size()), writes.data());
        vkCmdPushConstants(
            commands, pipeline.getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
        vkCmdDispatch(commands, groupsFor(target.getWidth()), groupsFor(target.getHeight()), 1);
    }

    void BloomPass::record(VkCommandBuffer commands, const Image& frame) const
    {
        assert((frame.getUsage() & VK_IMAGE_USAGE_SAMPLED_BIT) != 0 && "the pyramid samples the frame");

        // A frame too small for even one level has no pyramid, which `getPyramid` says and the
        // display pass reads as no lens at all. Every dispatch below would be over no pixels.
        if (mLevels.empty())
            return;

        assert(mLevels.front()->getWidth() == frame.getWidth() / 2 && "record before resize");

        // Nothing has written the levels yet this frame, so the halvings may discard whatever the
        // last one left.
        for (const std::unique_ptr<Image>& level : mLevels)
            level->transition(commands, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

        const Image* source = &frame;
        for (const std::unique_ptr<Image>& level : mLevels)
        {
            run(commands, mHalvePipeline, *source, *level, 0.0f);
            handOver(commands, *level);
            source = level.get();
        }

        // Back up the pyramid, each level mixed into the one above it. The coarsest has nothing
        // coarser to take, which is why this starts one below the end.
        for (std::size_t level = mLevels.size() - 1; level > 0; --level)
        {
            const Image& finer = *mLevels[level - 1];

            // The finer level is about to be read as well as written, and what it holds is its own
            // halving from the loop above — a write after a read after a write, all in one stage.
            finer.transition(commands, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

            run(commands, mSpreadPipeline, *mLevels[level], finer, Shaders::BLOOM_SCATTER);
            handOver(commands, finer);
        }
    }
}
