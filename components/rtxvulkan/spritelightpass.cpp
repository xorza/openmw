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
        constexpr std::array<VkDescriptorSetLayoutBinding, 2> sBindings{
            computeBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
            computeBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
        };

        /// The shader's own workgroup.
        constexpr std::uint32_t sWorkgroup = 16;
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
            DescriptorWrites<2> writes;
            writes.image(0, source.describeSampled(sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            writes.image(1, bake.describeStorage(level));

            const Shaders::SpriteLightConstants constants{
                .mLevel = level,
                .mWidth = bake.getWidthAt(level),
                .mHeight = bake.getHeightAt(level),
            };

            dispatch(commands, mPipeline, writes.get(), constants, groupsFor(constants.mWidth, sWorkgroup),
                groupsFor(constants.mHeight, sWorkgroup));
        }

        bake.transition(commands, Use::sComputeWrite, Use::sTextureSample);
    }
}
