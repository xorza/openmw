#include "tonepass.hpp"

#include <array>
#include <cassert>

#include <osg/Vec2f>

#include <components/rtx/shaders/bloom.h>
#include <components/rtx/shaders/look.h>
#include <components/rtx/shaders/tone.h>

#include "dispatch.hpp"
#include "formats.hpp"
#include "image.hpp"
#include "pipeline.hpp"

namespace Rtx
{
    namespace
    {
        /// The frame in, the picture out, what the star field is drawn through, the one float the
        /// curve scales by, the bloom pyramid the lens is spread from, and the one float the glare
        /// fader is laid on by. All pushed.
        constexpr std::array<VkDescriptorSetLayoutBinding, Shaders::TONE_BINDINGS> sBindings{
            computeBinding(Shaders::TONE_BIND_COLOUR, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            computeBinding(Shaders::TONE_BIND_TARGET, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            computeBinding(Shaders::TONE_BIND_STARS_SHOWN, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            computeBinding(Shaders::TONE_BIND_EXPOSURE, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
            computeBinding(Shaders::TONE_BIND_BLOOM, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
            computeBinding(Shaders::TONE_BIND_SUN_GLARE, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
            computeBinding(Shaders::TONE_BIND_PUFFS_DEPTH, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
        };
    }

    TonePass::TonePass(
        const Device& device, VkDescriptorSetLayout textureLayout, const std::filesystem::path& shaderDirectory)
        : mPipeline(device, sBindings, sizeof(Shaders::ToneConstants), SharedSetLayouts{ .mTextures = textureLayout },
            shaderDirectory / "tone.comp.spv", "tone")
        , mSampler(makeTargetSampler(device, "tone"))
        , mNoBloom(makeStandIn(device, toVulkanFormat(BLOOM_LEVEL), VK_IMAGE_USAGE_SAMPLED_BIT, "no-bloom"))
    {
    }

    void TonePass::record(VkCommandBuffer commands, const Tone& what) const
    {
        const Image& colour = what.mColour;
        const Buffer& exposure = what.mExposure;
        const Buffer& sunGlare = what.mSunGlare;
        const Image& starsShown = what.mStarsShown;
        const Image& puffsDepth = what.mPuffsDepth;
        const Image* const bloom = what.mBloom;
        const VkDescriptorSet textures = what.mTextures;
        const Image& target = what.mTarget;
        Shaders::ToneConstants constants = what.mConstants;

        assert(constants.mCamera.mWidth <= target.getWidth() && constants.mCamera.mHeight <= target.getHeight());

        // Set from whether there is a pyramid, rather than asked of the caller. A strength with
        // no pyramid behind it is a sampled stand-in mixed into the picture, and the one place that
        // knows which was bound is here.
        const Image& spread = bloom != nullptr ? *bloom : mNoBloom;
        constants.mBloom = bloom != nullptr ? Shaders::BLOOM_STRENGTH : 0.0f;
        constants.mBloomTexel
            = osg::Vec2f(1.0f / static_cast<float>(spread.getWidth()), 1.0f / static_cast<float>(spread.getHeight()));

        DescriptorWrites<Shaders::TONE_BINDINGS> writes;
        writes.image(Shaders::TONE_BIND_COLOUR, colour.describeStorage());
        writes.image(Shaders::TONE_BIND_TARGET, target.describeStorage());
        writes.image(Shaders::TONE_BIND_STARS_SHOWN, starsShown.describeStorage());
        writes.buffer(Shaders::TONE_BIND_EXPOSURE, exposure.describe());
        writes.image(Shaders::TONE_BIND_BLOOM, spread.describeSampled(mSampler.get()),
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        writes.buffer(Shaders::TONE_BIND_SUN_GLARE, sunGlare.describe());
        writes.image(Shaders::TONE_BIND_PUFFS_DEPTH, puffsDepth.describeStorage());

        // The scene's textures before the launch and beside the pushed set, which the two are
        // independent of: a pushed set and a bound one only have to be in place by the dispatch.
        bindSets(commands, mPipeline, SharedSetBinds{ .mTextures = textures });

        dispatch(commands, mPipeline, writes.get(), constants,
            groupsFor(constants.mCamera.mWidth, Shaders::TONE_WORKGROUP),
            groupsFor(constants.mCamera.mHeight, Shaders::TONE_WORKGROUP));
    }
}
