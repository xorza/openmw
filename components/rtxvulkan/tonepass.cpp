#include "tonepass.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <span>

#include <components/rtx/shaders/bloom.h>
#include <components/rtx/shaders/look.h>

#include "dispatch.hpp"
#include "image.hpp"

namespace Rtx
{
    namespace
    {
        /// The frame in, the picture out, what the star field is drawn through, the one float the
        /// curve scales by, and the bloom pyramid the lens is spread from. All pushed.
        constexpr std::array<VkDescriptorSetLayoutBinding, 5> sBindings{
            computeBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            computeBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            computeBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            computeBinding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
            computeBinding(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
        };
    }

    TonePass::TonePass(const Device& device, CommandPool& pool, VkDescriptorSetLayout textureLayout,
        const std::filesystem::path& shaderDirectory)
        : mPipeline(device, sBindings, sizeof(Shaders::ToneConstants), std::span(&textureLayout, 1),
            shaderDirectory / "tone.comp.spv", "tone")
        , mSampler(makeTargetSampler(device, "tone"))
        , mNoBloom(makeStandIn(device, pool, BLOOM_LEVEL, VK_IMAGE_USAGE_SAMPLED_BIT, "no-bloom"))
    {
    }

    void TonePass::record(VkCommandBuffer commands, const Image& colour, VkBuffer exposure, const Image& starsShown,
        const Image* bloom, VkDescriptorSet textures, const Image& target, Shaders::ToneConstants constants) const
    {
        assert(constants.mWidth <= target.getWidth() && constants.mHeight <= target.getHeight());

        // **Set from whether there is a pyramid, rather than asked of the caller.** A strength with
        // no pyramid behind it is a sampled stand-in mixed into the picture, and the one place that
        // knows which was bound is here.
        const Image& spread = bloom != nullptr ? *bloom : mNoBloom;
        constants.mBloom = bloom != nullptr ? Shaders::BLOOM_STRENGTH : 0.0f;
        constants.mBloomTexel
            = osg::Vec2f(1.0f / static_cast<float>(spread.getWidth()), 1.0f / static_cast<float>(spread.getHeight()));

        const std::array<VkDescriptorImageInfo, 3> images{
            VkDescriptorImageInfo{ VK_NULL_HANDLE, colour.getView(), VK_IMAGE_LAYOUT_GENERAL },
            VkDescriptorImageInfo{ VK_NULL_HANDLE, target.getView(), VK_IMAGE_LAYOUT_GENERAL },
            VkDescriptorImageInfo{ VK_NULL_HANDLE, starsShown.getView(), VK_IMAGE_LAYOUT_GENERAL },
        };
        const VkDescriptorBufferInfo scale{ exposure, 0, VK_WHOLE_SIZE };
        const VkDescriptorImageInfo pyramid{ mSampler.get(), spread.getView(), VK_IMAGE_LAYOUT_GENERAL };

        // The first three are storage images and the last two are not, so the shared filler covers
        // the front of the set and the two below name themselves.
        std::array<VkWriteDescriptorSet, 5> writes{};
        const std::array<VkWriteDescriptorSet, 3> stored = storageImageWrites(images);
        std::copy(stored.begin(), stored.end(), writes.begin());

        writes[3] = bufferWrite(3, scale);
        writes[4] = imageWrite(4, pyramid, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);

        // **The scene's textures before the launch and beside set zero**, which the two are
        // independent of: a pushed set and a bound one only have to be in place by the dispatch.
        vkCmdBindDescriptorSets(
            commands, VK_PIPELINE_BIND_POINT_COMPUTE, mPipeline.getLayout(), 1, 1, &textures, 0, nullptr);

        dispatch(commands, mPipeline, writes, constants, groupsFor(constants.mWidth, Shaders::TONE_WORKGROUP),
            groupsFor(constants.mHeight, Shaders::TONE_WORKGROUP));
    }
}
