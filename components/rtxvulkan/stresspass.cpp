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
        constexpr std::array<VkDescriptorSetLayoutBinding, Shaders::STRESS_BINDINGS> sBindings
            = computeBindings<Shaders::STRESS_BINDINGS>(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    }

    StressPass::StressPass(
        const Device& device, const std::filesystem::path& shaderDirectory, const double milliseconds)
        : mPipeline(
            device, sBindings, sizeof(Shaders::StressConstants), {}, shaderDirectory / "stress.comp.spv", "stress")
        , mNanoseconds(static_cast<std::uint32_t>(std::llround(milliseconds * 1.0e6)))
    {
    }

    void StressPass::record(VkCommandBuffer commands, GpuTimer& timer, const Buffer& counts)
    {
        timer.open(commands, RenderProfile::sHoldZone);

        DescriptorWrites<Shaders::STRESS_BINDINGS> writes;
        writes.buffer(Shaders::STRESS_BIND_COUNTS, counts.describe());
        dispatch(commands, mPipeline, writes.get(), Shaders::StressConstants{ .mNanoseconds = mNanoseconds }, 1);

        timer.close(commands);
    }
}
