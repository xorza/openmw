#include "tonepass.hpp"

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

    void TonePass::record(VkCommandBuffer commands, const Image& colour, const Buffer& exposure,
        const Image& starsShown, const Image* bloom, VkDescriptorSet textures, const Image& target,
        Shaders::ToneConstants constants) const
    {
        assert(constants.mWidth <= target.getWidth() && constants.mHeight <= target.getHeight());

        // Set from whether there is a pyramid, rather than asked of the caller. A strength with
        // no pyramid behind it is a sampled stand-in mixed into the picture, and the one place that
        // knows which was bound is here.
        const Image& spread = bloom != nullptr ? *bloom : mNoBloom;
        constants.mBloom = bloom != nullptr ? Shaders::BLOOM_STRENGTH : 0.0f;
        constants.mBloomTexel
            = osg::Vec2f(1.0f / static_cast<float>(spread.getWidth()), 1.0f / static_cast<float>(spread.getHeight()));

        DescriptorWrites<5> writes;
        writes.image(0, colour.describeStorage());
        writes.image(1, target.describeStorage());
        writes.image(2, starsShown.describeStorage());
        writes.buffer(3, exposure.describe());
        writes.image(4, spread.describeSampled(mSampler.get()), VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);

        // The scene's textures before the launch and beside set zero, which the two are
        // independent of: a pushed set and a bound one only have to be in place by the dispatch.
        bindSets(commands, mPipeline, std::span(&textures, 1));

        dispatch(commands, mPipeline, writes.get(), constants, groupsFor(constants.mWidth, Shaders::TONE_WORKGROUP),
            groupsFor(constants.mHeight, Shaders::TONE_WORKGROUP));
    }
}
