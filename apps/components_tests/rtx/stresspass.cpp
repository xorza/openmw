#include <cstdint>

#include <gtest/gtest.h>

#include <vulkan/vulkan_core.h>

#include <components/rtx/renderer.hpp>
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
            /// One frame of `hold`: recorded, waited out, read, and the reading handed back.
            double frameOf(StressPass& hold, GpuTimer& timer, const std::uint64_t frame)
            {
                timer.beginFrame(frame);
                getDevice().getPool().submitAndWait(
                    [&](VkCommandBuffer commands) { hold.record(commands, timer, frame); });

                GpuZones zones;
                timer.resolve(zones);
                if (zones.spans().empty())
                    return 0.0;

                const double held = zones.spans().front().mMs;
                hold.follow(frame, held);
                return held;
            }
        };

        /// The hold comes to what was asked from the second frame on, whatever the card's clock
        /// and whatever count it started from, and two holds asked for different times hold
        /// different times. A quarter either way, which is what `Check::QueueHeld` allows a run:
        /// each frame here is waited out, so the card is between clocks from one to the next.
        TEST_F(RtxStressPassTest, theHoldComesToWhatWasAskedFromTheCardsOwnRate)
        {
            Device& device = getDevice();
            GpuTimer timer(device);

            StressPass four(device, Testing::getShaderDirectory(), 4.0);
            StressPass eight(device, Testing::getShaderDirectory(), 8.0);

            // The first frame runs the count the pass starts from, which answers nothing; its
            // reading is the rate every frame after runs at.
            const double first = frameOf(four, timer, 0);
            if (first <= 0.0)
                GTEST_SKIP() << "the device does not time its own zones";

            for (std::uint64_t frame = 1; frame < 5; ++frame)
                EXPECT_NEAR(frameOf(four, timer, frame), 4.0, 1.0) << "frame " << frame;

            frameOf(eight, timer, 0);
            for (std::uint64_t frame = 1; frame < 5; ++frame)
                EXPECT_NEAR(frameOf(eight, timer, frame), 8.0, 2.0) << "frame " << frame;
        }
    }
}
