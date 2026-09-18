#include "stresspass.hpp"

#include <algorithm>
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
        , mSink(Buffer::deviceLocal(device, sizeof(std::uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "stress sink"))
        , mAskedMs(milliseconds)
    {
    }

    void StressPass::record(VkCommandBuffer commands, GpuTimer& timer, const std::uint64_t frame)
    {
        mCounts[frame % sRemembered] = mIterations;

        timer.open(commands, RenderProfile::sHoldZone);

        DescriptorWrites<1> writes;
        writes.buffer(0, mSink.describe());
        dispatch(commands, mPipeline, writes.get(), Shaders::StressConstants{ .mIterations = mIterations }, 1);

        timer.close(commands);
    }

    void StressPass::follow(const std::uint64_t frame, const double heldMs)
    {
        // A device that cannot time itself reads nought, and a count corrected by nothing stays.
        const std::uint32_t ran = mCounts[frame % sRemembered];
        if (!(heldMs > 0.0) || ran == 0)
            return;

        // Half of each reading, because one reading is one clock: a frame the card ran a notch
        // faster read a sixth short and put the next one a sixth long, and half of that is inside
        // what the clock moves by anyway.
        const double read = heldMs / ran;
        mMsPerIteration = mMsPerIteration > 0.0 ? 0.5 * (mMsPerIteration + read) : read;
        mIterations = static_cast<std::uint32_t>(std::clamp(std::round(mAskedMs / mMsPerIteration), 1.0, 4.0e9));
    }
}
