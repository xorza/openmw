#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include <apps/rtxtool/gpuclock.hpp>

namespace RtxTool
{
    namespace
    {
        /// The mask the driver answers with, in the words a report prints.
        ///
        /// **The bits are NVML's and this file is the only place that knows them**, so a wrong one
        /// is a report that names the wrong reason for a slow run — and the reasons are exactly what
        /// a number is quoted against. Each case below is a bit that means something different about
        /// what a measurement is worth.
        TEST(RtxGpuClockTest, aThrottleMaskReadsAsWhatIsHoldingTheCardBack)
        {
            EXPECT_EQ(describeThrottle(0x0000000000000000ull), "") << "nothing holds a card at rest";

            // The one this machine reports through a bench, and the reason its numbers are taken at
            // 1.8 GHz rather than the 2.3 the card reaches cool.
            EXPECT_EQ(describeThrottle(0x0000000000000004ull), "sw power cap");

            // The one that says a reading is worthless: an idle card is not the card the frames
            // above were drawn on.
            EXPECT_EQ(describeThrottle(0x0000000000000001ull), "gpu idle");

            EXPECT_EQ(describeThrottle(0x0000000000000020ull), "sw thermal slowdown");
            EXPECT_EQ(describeThrottle(0x0000000000000080ull), "hw power brake");

            // Several at once, in the order the bits are numbered rather than the order they were
            // asked about.
            EXPECT_EQ(describeThrottle(0x0000000000000044ull), "sw power cap, hw thermal slowdown");

            // **A bit this does not know still says something.** A driver that grows a reason must
            // read as an unknown one rather than as a card nothing is holding back, which is the
            // difference between "this number is suspect" and "this number is clean".
            EXPECT_EQ(describeThrottle(0x0000000000001000ull), "unknown reason 0x0000000000001000");
            EXPECT_EQ(describeThrottle(0x0000000000001004ull), "sw power cap, unknown reason 0x0000000000001000");
        }

        /// The line a report carries, and the silence where there is nothing to carry.
        TEST(RtxGpuClockTest, aClockNothingAnsweredForPrintsNothing)
        {
            // A machine with no `nvidia-smi` reports no clock rather than one of zero megahertz,
            // which would read as a measurement rather than as an absence.
            EXPECT_EQ(describeClock(GpuClock{}), "");

            const GpuClock capped{ .mLowestMhz = 1785,
                .mHighestMhz = 1785,
                .mMemoryMhz = 9001,
                .mTemperatureC = 66,
                .mThrottleMask = 0x4,
                .mRead = true };
            EXPECT_EQ(describeClock(capped), "  clock 1785 MHz core, 9001 MHz memory, 66 °C — sw power cap\n");

            // A card at its own clock says so in words, rather than trailing an empty dash.
            const GpuClock free{
                .mLowestMhz = 2325, .mHighestMhz = 2325, .mMemoryMhz = 9001, .mTemperatureC = 43, .mRead = true
            };
            EXPECT_EQ(describeClock(free), "  clock 2325 MHz core, 9001 MHz memory, 43 °C — nothing holding it back\n");
        }

        /// A place is bounded by its two readings rather than described by its last.
        ///
        /// **The reason the second reading exists.** A card sampled after the frames stopped is one
        /// already climbing back off the load — measured here at 2325 MHz against the 1770–1875 a
        /// bench actually runs at — so a single reading prints a fast clock over frames drawn at a
        /// slower one, which is worse than printing none.
        TEST(RtxGpuClockTest, twoReadingsBoundTheFramesBetweenThem)
        {
            GpuClock place{ .mLowestMhz = 1785,
                .mHighestMhz = 1785,
                .mMemoryMhz = 9001,
                .mTemperatureC = 61,
                .mThrottleMask = 0x4,
                .mRead = true };

            place.add(GpuClock{ .mLowestMhz = 2070,
                .mHighestMhz = 2070,
                .mMemoryMhz = 9001,
                .mTemperatureC = 66,
                .mThrottleMask = 0x1,
                .mRead = true });

            EXPECT_EQ(place.mLowestMhz, 1785u);
            EXPECT_EQ(place.mHighestMhz, 2070u);

            // The hottest it got, and every reason either end saw — a card that went idle at one end
            // and was capped at the other was both, and each says something about the numbers.
            EXPECT_EQ(place.mTemperatureC, 66u);
            EXPECT_EQ(
                describeClock(place), "  clock 1785–2070 MHz core, 9001 MHz memory, 66 °C — gpu idle, sw power cap\n");

            // **A reading nothing answered adds nothing**, so one end failing leaves the other end's
            // figure standing rather than pulling the range down to zero.
            const GpuClock held = place;
            place.add(GpuClock{});
            EXPECT_EQ(place.mLowestMhz, held.mLowestMhz);
            EXPECT_EQ(place.mHighestMhz, held.mHighestMhz);

            // And the first reading into an empty one is that reading, rather than a range from
            // nought.
            GpuClock first;
            first.add(held);
            EXPECT_TRUE(first.mRead);
            EXPECT_EQ(first.mLowestMhz, held.mLowestMhz);
        }

        /// What the tool says on this machine, where it is installed at all.
        ///
        /// **A skip and not a failure where nothing answers**: the harness runs on machines without
        /// an NVIDIA driver, and a clock is instrumentation rather than a renderer.
        TEST(RtxGpuClockTest, theCardAnswersWithAClockItCouldBeRunningAt)
        {
            const GpuClock clock = readGpuClock();
            if (!clock.mRead)
                GTEST_SKIP() << "nothing answered for a GPU clock on this machine";

            // A graphics clock and a memory clock a card of the last decade could hold, which is
            // what says the fields were read in the order they were asked for rather than shuffled.
            EXPECT_GT(clock.mLowestMhz, 100u);
            EXPECT_LT(clock.mLowestMhz, 10000u);
            EXPECT_EQ(clock.mLowestMhz, clock.mHighestMhz) << "one reading is not a range";
            EXPECT_GT(clock.mMemoryMhz, 100u);
            EXPECT_GT(clock.mTemperatureC, 0u);
            EXPECT_LT(clock.mTemperatureC, 120u);

            EXPECT_FALSE(describeClock(clock).empty());
        }
    }
}
