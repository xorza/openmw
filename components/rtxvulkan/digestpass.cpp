#include "digestpass.hpp"

#include <array>
#include <cassert>
#include <cstddef>

#include <components/rtx/shaders/digest.h>

#include "buffer.hpp"
#include "dispatch.hpp"
#include "gputimer.hpp"
#include "image.hpp"
#include "imageuse.hpp"

namespace Rtx
{
    namespace
    {
        constexpr std::array<VkDescriptorSetLayoutBinding, Shaders::DIGEST_BINDINGS> sBindings{
            VkDescriptorSetLayoutBinding{ Shaders::DIGEST_BIND_IMAGES, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                Shaders::DIGEST_IMAGES, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            computeBinding(Shaders::DIGEST_BIND_LANES, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
        };
    }

    DigestPass::DigestPass(const Device& device, const std::filesystem::path& shaderDirectory)
        : mPipeline(
            device, sBindings, sizeof(Shaders::DigestConstants), {}, shaderDirectory / "digest.comp.spv", "digest")
        , mLanes(Buffer::deviceLocal(device, sBytes,
              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
              "digest lanes"))
    {
    }

    void DigestPass::record(const VkCommandBuffer commands,
        const std::array<const Image*, Shaders::DIGEST_IMAGES>& images, const Buffer& into, GpuTimer* const timer) const
    {
        assert(into.getSize() >= sBytes && "a digest of more images than the frame has room for");

        const Image& first = *images.front();
        std::array<VkDescriptorImageInfo, Shaders::DIGEST_IMAGES> described{};
        for (std::size_t at = 0; at < images.size(); ++at)
        {
            assert(images[at]->getWidth() == first.getWidth() && images[at]->getHeight() == first.getHeight()
                && "a digest of images at two extents");
            described[at] = images[at]->describeStorage();
        }

        openZone(timer, commands, "digest");

        // Cleared on the queue; the last frame's copy out of it is behind the head barrier
        // `CommandPool::begin` recorded.
        mLanes.clear(commands);
        mLanes.transition(commands, Use::sBufferClearWrite, Use::sBufferComputeReadWrite);

        DescriptorWrites<Shaders::DIGEST_BINDINGS, Shaders::DIGEST_IMAGES> writes;
        writes.images(Shaders::DIGEST_BIND_IMAGES, described, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        writes.buffer(Shaders::DIGEST_BIND_LANES, mLanes.describe());

        const Shaders::DigestConstants constants{ .mWidth = first.getWidth(), .mHeight = first.getHeight() };
        dispatch(commands, mPipeline, writes.get(), constants, groupsFor(first.getWidth(), Shaders::DIGEST_WORKGROUP),
            groupsFor(first.getHeight(), Shaders::DIGEST_WORKGROUP));

        // The host's read of what `into` held before is behind the submit that carries this — a
        // host read finished before the queue took the commands needs no dependency of its own.
        mLanes.transition(commands, Use::sBufferComputeWrite, Use::sBufferCopyRead);
        mLanes.copyTo(commands, into, sBytes);

        into.orderForHostRead(commands);
        closeZone(timer, commands);
    }
}
