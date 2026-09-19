#include "digestpass.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

#include "buffer.hpp"
#include "dispatch.hpp"
#include "gputimer.hpp"
#include "image.hpp"
#include "imageuse.hpp"

namespace Rtx
{
    namespace
    {
        constexpr std::array<VkDescriptorSetLayoutBinding, 2> sBindings{
            VkDescriptorSetLayoutBinding{
                0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, Shaders::DIGEST_IMAGES, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            computeBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
        };
    }

    DigestPass::DigestPass(const Device& device, const std::filesystem::path& shaderDirectory)
        : mPipeline(
            device, sBindings, sizeof(Shaders::DigestConstants), {}, shaderDirectory / "digest.comp.spv", "digest")
    {
    }

    void DigestPass::record(const VkCommandBuffer commands,
        const std::array<const Image*, Shaders::DIGEST_IMAGES>& images, const Buffer& lanes,
        GpuTimer* const timer) const
    {
        assert(lanes.getSize() >= images.size() * Shaders::DIGEST_LANES * sizeof(std::uint32_t)
            && "a digest of more images than the lanes have room for");

        const Image& first = *images.front();
        std::array<VkDescriptorImageInfo, Shaders::DIGEST_IMAGES> described{};
        for (std::size_t at = 0; at < images.size(); ++at)
        {
            assert(images[at]->getWidth() == first.getWidth() && images[at]->getHeight() == first.getHeight()
                && "a digest of images at two extents");
            described[at] = images[at]->describeStorage();
        }

        openZone(timer, commands, "digest");

        // Cleared on the queue rather than by the host, because the frame that last used this
        // buffer may still be in flight when this one is recorded.
        lanes.transition(commands, Use::sBufferHostRead, Use::sBufferClearWrite);
        lanes.clear(commands);
        lanes.transition(commands, Use::sBufferClearWrite, Use::sBufferComputeReadWrite);

        DescriptorWrites<2, Shaders::DIGEST_IMAGES> writes;
        writes.images(0, described, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        writes.buffer(1, lanes.describe());

        const Shaders::DigestConstants constants{ .mWidth = first.getWidth(), .mHeight = first.getHeight() };
        dispatch(commands, mPipeline, writes.get(), constants, groupsFor(first.getWidth(), Shaders::DIGEST_WORKGROUP),
            groupsFor(first.getHeight(), Shaders::DIGEST_WORKGROUP));

        lanes.orderForHostRead(commands);
        closeZone(timer, commands);
    }
}
