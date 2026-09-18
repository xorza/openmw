#include "mipchainpass.hpp"

#include <array>
#include <cassert>

#include <components/rtx/shaders/mipchain.h>

#include "dispatch.hpp"
#include "image.hpp"
#include "imageuse.hpp"

namespace Rtx
{
    namespace
    {
        /// The upload in, the level above in, the level written out.
        constexpr std::array<VkDescriptorSetLayoutBinding, 3> sBindings{
            computeBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
            computeBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            computeBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
        };
    }

    MipChainPass::MipChainPass(const Device& device, const std::filesystem::path& shaderDirectory)
        : mPipeline(device, sBindings, sizeof(Shaders::MipChainConstants), {}, shaderDirectory / "mipchain.comp.spv",
            "mip chain")
    {
    }

    void MipChainPass::record(const VkCommandBuffer commands, const Image& source, const VkSampler sampler,
        const Image& chain, const bool encoded) const
    {
        assert(source.getMipLevels() == 1 && "a chain is built for a file that carried none");
        assert(chain.getWidth() == source.getWidth() && chain.getHeight() == source.getHeight()
            && "a chain shaped unlike its source");

        // Read and written, from the first dispatch on: every dispatch binds the level above the one
        // it writes, the first bound to the level it writes and reading it not at all — which the
        // layers cannot see, so the transition makes it readable too rather than leaving a hazard
        // on the record.
        chain.transition(commands, Use::sUndefined, Use::sComputeReadWrite);

        for (std::uint32_t level = 0; level < chain.getMipLevels(); ++level)
        {
            // Every level but the first reads the one written just before it: one barrier between
            // two dispatches over one image, and the image stays where a dispatch reads and writes.
            if (level > 0)
                chain.transition(commands, Use::sComputeReadWrite, Use::sComputeReadWrite);

            DescriptorWrites<3> writes;
            writes.image(0, source.describeSampled(sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            writes.image(1, chain.describeStorage(level > 0 ? level - 1 : 0));
            writes.image(2, chain.describeStorage(level));

            const Shaders::MipChainConstants constants{
                .mLevel = level,
                .mWidth = chain.getWidthAt(level),
                .mHeight = chain.getHeightAt(level),
                .mEncoded = encoded ? 1u : 0u,
            };

            dispatch(commands, mPipeline, writes.get(), constants,
                groupsFor(constants.mWidth, Shaders::MIP_CHAIN_WORKGROUP),
                groupsFor(constants.mHeight, Shaders::MIP_CHAIN_WORKGROUP));
        }

        chain.transition(commands, Use::sComputeReadWrite, Use::sTextureSample);
    }
}
