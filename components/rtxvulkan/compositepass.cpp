#include "compositepass.hpp"

#include <array>
#include <cassert>

#include <components/rtx/frameimage.hpp>
#include <components/rtx/shaders/composite.h>

#include "dispatch.hpp"
#include "gbuffer.hpp"
#include "image.hpp"

namespace Rtx
{
    namespace
    {
        /// Three channels in, the running sum, and the frame out — all storage images, all pushed.
        constexpr std::array<VkDescriptorSetLayoutBinding, Shaders::COMPOSITE_BINDINGS> sBindings
            = computeBindings<Shaders::COMPOSITE_BINDINGS>(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    }

    CompositePass::CompositePass(const Device& device, const std::filesystem::path& shaderDirectory)
        : mPipeline(device, sBindings, sizeof(Shaders::CompositeConstants), {}, shaderDirectory / "composite.comp.spv",
            "composite")
        , mNoSum(makeStandIn(device, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT, "no-sum"))
    {
    }

    void CompositePass::record(VkCommandBuffer commands, const GBuffer& buffer, const Image& indirect, const Image* sum,
        const Image& colour, const Shaders::CompositeConstants& constants) const
    {
        assert(buffer.getWidth() >= constants.mWidth && buffer.getHeight() >= constants.mHeight);
        assert(indirect.getWidth() >= constants.mWidth && indirect.getHeight() >= constants.mHeight);
        assert(colour.getWidth() >= constants.mWidth && colour.getHeight() >= constants.mHeight);

        // A sum has to cover the frame it is a sum of; a stand-in never read does not.
        assert(constants.mAccumulate == 0 || sum != nullptr);
        assert(sum == nullptr || (sum->getWidth() >= constants.mWidth && sum->getHeight() >= constants.mHeight));

        // The real sum is the caller's to order; the stand-in is touched by one composite a
        // command buffer, and the head barrier `CommandPool::begin` recorded orders that after the
        // last one.
        const Image& bound = sum != nullptr ? *sum : mNoSum;

        DescriptorWrites<Shaders::COMPOSITE_BINDINGS> writes;
        writes.image(Shaders::COMPOSITE_BIND_DIRECT, buffer.get(Channel::Direct).describeStorage());
        writes.image(Shaders::COMPOSITE_BIND_INDIRECT, indirect.describeStorage());
        writes.image(Shaders::COMPOSITE_BIND_ALBEDO, buffer.get(Channel::Albedo).describeStorage());
        writes.image(Shaders::COMPOSITE_BIND_SUM, bound.describeStorage());
        writes.image(Shaders::COMPOSITE_BIND_COLOUR, colour.describeStorage());

        dispatch(commands, mPipeline, writes.get(), constants,
            groupsFor(constants.mWidth, Shaders::COMPOSITE_WORKGROUP),
            groupsFor(constants.mHeight, Shaders::COMPOSITE_WORKGROUP));
    }
}
