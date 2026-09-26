#include "groundcompositepass.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>

#include <components/rtx/shaders/ground.h>
#include <components/rtx/shaders/scene.h>

#include "dispatch.hpp"
#include "formats.hpp"
#include "image.hpp"
#include "imageuse.hpp"
#include "pipeline.hpp"

namespace Rtx
{
    namespace
    {
        /// The two finest levels, out.
        constexpr std::array<VkDescriptorSetLayoutBinding, Shaders::GROUND_COMPOSITE_BINDINGS> sBindings{
            computeBinding(Shaders::GROUND_COMPOSITE_BIND_ALBEDO, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            computeBinding(Shaders::GROUND_COMPOSITE_BIND_GLOSS, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
        };
    }

    GroundCompositePass::GroundCompositePass(
        const Device& device, const std::filesystem::path& shaderDirectory, const VkDescriptorSetLayout textures)
        : mPipeline(device, sBindings, sizeof(Shaders::GroundCompositeConstants),
            SharedSetLayouts{ .mTextures = textures }, shaderDirectory / "groundcomposite.comp.spv", "ground composite")
        , mNoTarget(makeStandIn(
              device, toVulkanFormat(TEXTURE_WRITTEN_FORMAT), VK_IMAGE_USAGE_STORAGE_BIT, "no ground target"))
    {
    }

    void GroundCompositePass::record(const VkCommandBuffer commands, const VkDescriptorSet textures,
        const Image* const albedo, const Image* const gloss, const Shaders::GroundCompositeConstants& chunk) const
    {
        assert((albedo != nullptr) == ((chunk.mOutputs & Shaders::GROUND_COMPOSITE_ALBEDO) != 0u)
            && (gloss != nullptr) == ((chunk.mOutputs & Shaders::GROUND_COMPOSITE_GLOSS) != 0u)
            && "a bake told to write an image it was not handed, or handed one it was not told to write");

        std::array<const Image*, 2> written{};
        std::size_t count = 0;
        for (const Image* image : { albedo, gloss })
        {
            if (image == nullptr)
                continue;

            assert(image->getWidth() == Shaders::GROUND_COMPOSITE_EXTENT
                && image->getHeight() == Shaders::GROUND_COMPOSITE_EXTENT && "a composite of another size");
            image->transition(commands, Use::sUndefined, Use::sComputeWrite);
            written[count++] = image;
        }

        // The stand-in is written by no bake, but a layer that reads the shader's bindings and not its
        // branches counts every bake that binds it as a write. Two in a row are then a hazard it
        // reports, so the pair is ordered: a barrier on one texel, on a placement's few bakes.
        if (albedo == nullptr || gloss == nullptr)
            mNoTarget.transition(commands, Use::sComputeWrite, Use::sComputeWrite);

        DescriptorWrites<Shaders::GROUND_COMPOSITE_BINDINGS> writes;
        writes.image(
            Shaders::GROUND_COMPOSITE_BIND_ALBEDO, (albedo != nullptr ? *albedo : mNoTarget).describeStorage());
        writes.image(Shaders::GROUND_COMPOSITE_BIND_GLOSS, (gloss != nullptr ? *gloss : mNoTarget).describeStorage());

        // The scene's textures beside the pushed set, which the two are independent of: a pushed set
        // and a bound one only have to be in place by the dispatch.
        bindSets(commands, mPipeline, SharedSetBinds{ .mTextures = textures });

        constexpr std::uint32_t groups
            = groupsFor(Shaders::GROUND_COMPOSITE_EXTENT, Shaders::GROUND_COMPOSITE_WORKGROUP);
        dispatch(commands, mPipeline, writes.get(), chunk, groups, groups);

        // The chains, box filtered in light through each image's own format, which is the filter
        // the sum was made for; left where the array's sampler expects a texture.
        const std::span<const Image* const> baked(written.data(), count);
        Image::buildMips(commands, baked);
        for (const Image* image : baked)
            image->transition(commands, Use::sShaderSample, Use::sTextureSample);
    }
}
