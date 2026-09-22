#include "sunglarepass.hpp"

#include <array>

#include <components/rtx/shaders/glare.h>

#include "dispatch.hpp"
#include "imageuse.hpp"

namespace Rtx
{
    namespace
    {
        /// The counts in, the share out.
        constexpr std::array<VkDescriptorSetLayoutBinding, Shaders::SUN_GLARE_BINDINGS> sBindings
            = computeBindings<Shaders::SUN_GLARE_BINDINGS>(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
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
        // The previous frame's easing, which read the counts, is behind the head barrier
        // `CommandPool::begin` recorded; the launch that adds to them is behind the clear.
        mCounts.clear(commands);
        mCounts.transition(commands, Use::sBufferClearWrite,
            BufferUse{ VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT });
    }

    void SunGlarePass::record(const VkCommandBuffer commands, const float elapsedSeconds, const bool reset) const
    {
        // The counts the launch added to. The share this easing moves was last read by the
        // previous frame's curve and written by its easing, both behind the head barrier
        // `CommandPool::begin` recorded.
        mCounts.transition(commands,
            BufferUse{ VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT },
            Use::sBufferComputeRead);

        DescriptorWrites<Shaders::SUN_GLARE_BINDINGS> writes;
        writes.buffer(Shaders::SUN_GLARE_BIND_COUNT, mCounts.describe());
        writes.buffer(Shaders::SUN_GLARE_BIND_SHARE, mShare.describe());

        const Shaders::SunGlareConstants constants{
            .mElapsed = elapsedSeconds,
            .mReset = reset ? 1u : 0u,
        };

        dispatch(commands, mPipeline, writes.get(), constants, 1);

        // The curve reads what the easing wrote.
        mShare.transition(commands, Use::sBufferComputeWrite, Use::sBufferComputeRead);
    }
}
