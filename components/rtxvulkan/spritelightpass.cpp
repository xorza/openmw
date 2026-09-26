#include "spritelightpass.hpp"

#include <array>
#include <cassert>

#include <components/rtx/shaders/spritelight.h>

#include "dispatch.hpp"
#include "image.hpp"
#include "imageuse.hpp"

namespace Rtx
{
    namespace
    {
        /// The source in, one level of the bake out.
        constexpr std::array<VkDescriptorSetLayoutBinding, Shaders::SPRITE_LIGHT_BINDINGS> sBindings{
            computeBinding(Shaders::SPRITE_LIGHT_BIND_SOURCE, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
            computeBinding(Shaders::SPRITE_LIGHT_BIND_BAKE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
        };
    }

    SpriteLightPass::SpriteLightPass(const Device& device, const std::filesystem::path& shaderDirectory)
        : mPipeline(device, sBindings, sizeof(Shaders::SpriteLightConstants), {},
            shaderDirectory / "spritelight.comp.spv", "sprite light")
    {
    }

    void SpriteLightPass::record(
        const VkCommandBuffer commands, const Image& source, const VkSampler sampler, const Image& bake) const
    {
        assert(bake.getMipLevels() == source.getMipLevels() && "a bake shaped unlike its source");

        bake.transition(commands, Use::sUndefined, Use::sComputeWrite);

        for (std::uint32_t level = 0; level < bake.getMipLevels(); ++level)
        {
            DescriptorWrites<Shaders::SPRITE_LIGHT_BINDINGS> writes;
            writes.image(Shaders::SPRITE_LIGHT_BIND_SOURCE,
                source.describeSampled(sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            writes.image(Shaders::SPRITE_LIGHT_BIND_BAKE, bake.describeStorage(level));

            const Shaders::SpriteLightConstants constants{
                .mLevel = level,
                .mWidth = bake.getWidthAt(level),
                .mHeight = bake.getHeightAt(level),
            };

            dispatch(commands, mPipeline, writes.get(), constants,
                groupsFor(constants.mWidth, Shaders::SPRITE_LIGHT_WORKGROUP),
                groupsFor(constants.mHeight, Shaders::SPRITE_LIGHT_WORKGROUP));
        }

        bake.transition(commands, Use::sComputeWrite, Use::sTextureSample);
    }
}
