#include <cstdint>

#include <gtest/gtest.h>

#include <vulkan/vulkan_core.h>

#include <components/rtx/renderer.hpp>
#include <components/rtx/shaders/counts.h>
#include <components/rtxvulkan/buffer.hpp>
#include <components/rtxvulkan/commands.hpp>
#include <components/rtxvulkan/device.hpp>
#include <components/rtxvulkan/gputimer.hpp>
#include <components/rtxvulkan/stresspass.hpp>

#include "harness.hpp"

namespace Rtx
{
    namespace
    {
        struct RtxStressPassTest : Testing::DeviceTest
        {
            /// One frame of `hold`: recorded, waited out and read, as the zone measured it, in
            /// milliseconds. The loop's own reading lands in `counts`.
            double frameOf(StressPass& hold, GpuTimer& timer, Buffer& counts, const std::uint64_t frame)
            {
                timer.beginFrame(frame);
                getDevice().getPool().submitAndWait([&](VkCommandBuffer commands) {
                    hold.record(commands, timer, counts);
                    counts.orderForHostRead(commands);
                });

                GpuZones zones;
                timer.resolve(zones);
                if (zones.spans().empty())
                    return 0.0;

                return zones.spans().front().mMs;
            }
        };

        /// The hold is the time asked on every frame, the first included, whatever the card's clock:
        /// the loop's own clock reads what was asked and no more than one tick past it, and the
        /// queue was held at least that long.
        ///
        /// **Off a cold card, deliberately.** The card idles at a few hundred megahertz and steps
        /// its clock over the first frames of load, which is where a hold set by a count missed by
        /// a third. A clock switch stalls the card for a millisecond or so, which the zone shows
        /// and the loop's clock runs through; so the zone is asked only to be no shorter than the
        /// loop. Ten microseconds past the asked time is the tick the loop leaves on: the
        /// real-time clock advances by the microsecond.
        TEST_F(RtxStressPassTest, theHoldIsTheTimeAskedOnEveryFrameWhateverTheCardsClock)
        {
            Device& device = getDevice();
            GpuTimer timer(device);

            StressPass four(device, Testing::getShaderDirectory(), 4.0);
            StressPass eight(device, Testing::getShaderDirectory(), 8.0);

            // Where the loop leaves its reading, as the ring's frame slot holds it.
            Buffer counts = Buffer::readBack(
                device, sizeof(Shaders::FrameCounts), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "frame counts");

            struct Hold
            {
                StressPass* mPass;
                std::uint32_t mAskedNs;
            };

            for (const Hold hold : { Hold{ &four, 4000000u }, Hold{ &eight, 8000000u } })
                for (std::uint64_t frame = 0; frame < 8; ++frame)
                {
                    const double zoneMs = frameOf(*hold.mPass, timer, counts, frame);
                    if (zoneMs <= 0.0)
                        GTEST_SKIP() << "the device does not time its own zones";

                    const std::uint32_t held = static_cast<const Shaders::FrameCounts*>(counts.map())->mHeldNs;
                    EXPECT_GE(held, hold.mAskedNs) << "frame " << frame;
                    EXPECT_LE(held, hold.mAskedNs + 10000u) << "frame " << frame;
                    EXPECT_GE(zoneMs, static_cast<double>(held) * 1.0e-6) << "frame " << frame;
                }
        }
    }
}
