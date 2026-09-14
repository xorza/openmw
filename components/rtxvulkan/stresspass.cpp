#include "stresspass.hpp"

#include <algorithm>
#include <array>
#include <cmath>

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

        /// What the calibration runs: a count a card of this class takes about a millisecond over,
        /// repeated until this much device time has gone by, so the reading is taken at the clock
        /// the frames will run at rather than the one the card idles at.
        constexpr std::uint32_t sCalibrationIterations = 1u << 20;
        constexpr double sCalibrationMs = 100.0;
    }

    StressPass::StressPass(const Device& device, CommandPool& pool, const std::filesystem::path& shaderDirectory,
        const double milliseconds)
        : mPipeline(
            device, sBindings, sizeof(Shaders::StressConstants), {}, shaderDirectory / "stress.comp.spv", "stress")
        , mSink(Buffer::deviceLocal(device, sizeof(std::uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "stress sink"))
    {
        GpuTimer timer(device);
        double lastMs = 0.0;
        for (double spent = 0.0; spent < sCalibrationMs;)
        {
            mIterations = sCalibrationIterations;
            timer.beginFrame();
            pool.submitAndWait([&](VkCommandBuffer commands) { record(commands, timer); });

            GpuZones zones;
            timer.resolve(zones);
            lastMs = zones.spans().empty() ? 0.0 : zones.spans().front().mMs;

            // A device that cannot time itself cannot be calibrated against, and a loop that read
            // nought would never end: the stated count stands for the stated time.
            if (lastMs <= 0.0)
                break;

            spent += lastMs;
        }

        if (lastMs > 0.0)
            mIterations = static_cast<std::uint32_t>(
                std::clamp(std::round(milliseconds / lastMs * sCalibrationIterations), 1.0, 4.0e9));
    }

    void StressPass::record(VkCommandBuffer commands, GpuTimer& timer) const
    {
        timer.open(commands, "stress");

        DescriptorWrites<1> writes;
        writes.buffer(0, mSink.describe());
        dispatch(commands, mPipeline, writes.get(), Shaders::StressConstants{ .mIterations = mIterations }, 1);

        timer.close(commands);
    }
}
