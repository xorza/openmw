#include "sunglarepass.hpp"

#include <array>

#include <components/rtx/shaders/glare.h>

#include "barriers.hpp"
#include "dispatch.hpp"
#include "imageuse.hpp"

namespace Rtx
{
    namespace
    {
        /// The counts in, the share out.
        constexpr std::array<VkDescriptorSetLayoutBinding, 2> sBindings
            = computeBindings<2>(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    }

    SunGlarePass::SunGlarePass(const Device& device, const std::filesystem::path& shaderDirectory)
        : mPipeline(device, sBindings, sizeof(Shaders::SunGlareConstants), {}, shaderDirectory / "sunglare.comp.spv",
            "sun glare")
        , mCounts(Buffer::deviceLocal(device, sizeof(Shaders::SunGlareCount),
              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, "sun glare counts"))
        , mShare(Buffer::deviceLocal(device, sizeof(float),
              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, "sun glare share"))
        , mNoShare(Buffer::hostWritten(device, sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "no sun glare"))
    {
        mNoShare.writable<float>(0, 1).front() = 0.0f;
    }

    void SunGlarePass::begin(const VkCommandBuffer commands) const
    {
        // Against the previous frame's easing, which read the counts: an execution dependency is
        // all a write-after-read needs, and the launch that adds to them is behind the clear.
        mCounts.transition(commands, Use::sBufferComputeRead, Use::sBufferClearWrite);
        mCounts.clear(commands);
        mCounts.transition(commands, Use::sBufferClearWrite,
            BufferUse{ VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT });
    }

    void SunGlarePass::record(const VkCommandBuffer commands, const float elapsedSeconds, const bool reset) const
    {
        // The counts the launch added to, and the share the previous frame's curve read and this
        // easing moves — read as well as written, so the write before it has to be visible.
        Barriers before(commands);
        before.add(mCounts.describeBarrier(
            BufferUse{ VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT },
            Use::sBufferComputeRead));
        before.add(mShare.describeBarrier(Use::sBufferComputeReadWrite, Use::sBufferComputeReadWrite));
        before.flush();

        DescriptorWrites<2> writes;
        writes.buffer(0, mCounts.describe());
        writes.buffer(1, mShare.describe());

        const Shaders::SunGlareConstants constants{
            .mElapsed = elapsedSeconds,
            .mReset = reset ? 1u : 0u,
        };

        dispatch(commands, mPipeline, writes.get(), constants, 1);

        // The curve reads what the easing wrote.
        mShare.transition(commands, Use::sBufferComputeWrite, Use::sBufferComputeRead);
    }
}
