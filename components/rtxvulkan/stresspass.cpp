#include "stresspass.hpp"

#include <array>
#include <cmath>

#include <components/rtx/reconstruction.hpp>
#include <components/rtx/shaders/stress.h>

#include "commands.hpp"
#include "device.hpp"
#include "dispatch.hpp"
#include "gputimer.hpp"

namespace Rtx
{
    namespace
    {
        constexpr std::array<VkDescriptorSetLayoutBinding, 1> sBindings
            = computeBindings<1>(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    }

    StressPass::StressPass(
        const Device& device, const std::filesystem::path& shaderDirectory, const double milliseconds)
        : mPipeline(
            device, sBindings, sizeof(Shaders::StressConstants), {}, shaderDirectory / "stress.comp.spv", "stress")
        , mSink(Buffer::readBack(device, sizeof(std::uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "stress sink"))
        , mNanoseconds(static_cast<std::uint32_t>(std::llround(milliseconds * 1.0e6)))
    {
    }

    void StressPass::record(VkCommandBuffer commands, GpuTimer& timer)
    {
        timer.open(commands, RenderProfile::sHoldZone);

        DescriptorWrites<1> writes;
        writes.buffer(0, mSink.describe());
        dispatch(commands, mPipeline, writes.get(), Shaders::StressConstants{ .mNanoseconds = mNanoseconds }, 1);
        mSink.orderForHostRead(commands);

        timer.close(commands);
    }

    std::uint32_t StressPass::getHeldNs() const
    {
        return *static_cast<const std::uint32_t*>(mSink.map());
    }
}
