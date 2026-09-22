#include "groundcompositepass.hpp"

#include <array>
#include <cassert>

#include <components/rtx/shaders/ground.h>

#include "dispatch.hpp"
#include "image.hpp"
#include "imageuse.hpp"
#include "pipeline.hpp"

namespace Rtx
{
    namespace
    {
        /// The composite's finest level, out.
        constexpr std::array<VkDescriptorSetLayoutBinding, Shaders::GROUND_COMPOSITE_BINDINGS> sBindings{
            computeBinding(Shaders::GROUND_COMPOSITE_BIND_TARGET, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
        };
    }

    GroundCompositePass::GroundCompositePass(
        const Device& device, const std::filesystem::path& shaderDirectory, const VkDescriptorSetLayout textures)
        : mPipeline(device, sBindings, sizeof(Shaders::GroundCompositeConstants), std::array{ textures },
            shaderDirectory / "groundcomposite.comp.spv", "ground composite")
    {
    }

    void GroundCompositePass::record(const VkCommandBuffer commands, const VkDescriptorSet textures,
        const Image& composite, const Shaders::GroundCompositeConstants& chunk) const
    {
        assert(composite.getWidth() == Shaders::GROUND_COMPOSITE_EXTENT
            && composite.getHeight() == Shaders::GROUND_COMPOSITE_EXTENT && "a composite of another size");

        composite.transition(commands, Use::sUndefined, Use::sComputeWrite);

        DescriptorWrites<Shaders::GROUND_COMPOSITE_BINDINGS> writes;
        writes.image(Shaders::GROUND_COMPOSITE_BIND_TARGET, composite.describeStorage());

        // The scene's textures beside set zero, which the two are independent of: a pushed set
        // and a bound one only have to be in place by the dispatch.
        bindSets(commands, mPipeline, std::span(&textures, 1));

        constexpr std::uint32_t groups
            = groupsFor(Shaders::GROUND_COMPOSITE_EXTENT, Shaders::GROUND_COMPOSITE_WORKGROUP);
        dispatch(commands, mPipeline, writes.get(), chunk, groups, groups);

        // The chain, box filtered in light through the image's own format, which is the filter
        // the sum was made for; left where the array's sampler expects a texture.
        composite.buildMips(commands);
        composite.transition(commands, Use::sShaderSample, Use::sTextureSample);
    }
}
