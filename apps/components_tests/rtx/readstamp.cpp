#include <chrono>
#include <cstdint>

#include <gtest/gtest.h>

#include <vulkan/vulkan_core.h>

#include <components/rtxvulkan/commands.hpp>
#include <components/rtxvulkan/device.hpp>
#include <components/rtxvulkan/readstamp.hpp>
#include <components/rtxvulkan/timeline.hpp>

#include "harness.hpp"

namespace Rtx
{
    namespace
    {
        struct RtxReadStampTest : Testing::DeviceTest
        {
            /// An empty submit behind `hold`, and what it signals.
            std::uint64_t submitHeld(Testing::HeldSubmit& hold)
            {
                const VkCommandBuffer commands = getPool().allocate(1).front();
                getPool().begin(commands);
                return hold.submit(commands);
            }
        };

        /// **Two submits in flight are both seen.** Named for the first and the second, the stamp
        /// stays busy while either is on the queue; named again for the pending submit, the
        /// pending one is no hazard and the made ones still are.
        TEST_F(RtxReadStampTest, aStampNamedByTwoSubmitsInFlightReadsIdleOnlyOnceBothHaveRun)
        {
            const Device& device = getDevice();
            const Timeline& timeline = device.getTimeline();

            ReadStamp stamp;
            EXPECT_TRUE(stamp.isIdle(device)) << "nothing has named it";

            Testing::HeldSubmit first(device);
            Testing::HeldSubmit second(device);

            stamp.nameFor(timeline.getNext());
            const std::uint64_t one = submitHeld(first);
            stamp.nameFor(timeline.getNext());
            const std::uint64_t two = submitHeld(second);
            ASSERT_EQ(two, one + 1);
            EXPECT_EQ(stamp.getNamedUntil(), two);
            EXPECT_FALSE(stamp.isIdle(device)) << "both are on the queue";

            // Named for the submit not yet made, which is what a hand-out for the next frame is:
            // the first is still on the queue, and the stamp has to remember it.
            stamp.nameFor(timeline.getNext());
            EXPECT_FALSE(stamp.isIdle(device)) << "the newest naming is pending, the oldest is in flight";

            first.release();
            device.waitFor(one, "test");
            EXPECT_FALSE(stamp.isIdle(device)) << "the second is still on the queue";

            second.release();
            device.waitFor(two, "test");
            EXPECT_TRUE(stamp.isIdle(device)) << "every submit made has run, and the pending one is no hazard";

            // And a wait on a stamp whose newest naming is the pending submit waits for the
            // newest made and returns, rather than waiting for a value nothing signals.
            Testing::HeldSubmit third(device);
            stamp.nameFor(timeline.getNext());
            const std::uint64_t three = submitHeld(third);
            stamp.nameFor(timeline.getNext());
            EXPECT_FALSE(stamp.isIdle(device));
            third.releaseAfter(std::chrono::milliseconds(20));
            stamp.waitIdle(device, "test");
            EXPECT_TRUE(timeline.hasFinished(three));
            EXPECT_TRUE(stamp.isIdle(device));
        }
    }
}
