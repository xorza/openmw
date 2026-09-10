#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include <components/rtxbench/gpuclock.hpp>

namespace Rtx
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

            const GpuClock capped = GpuClock::reading(1785, 9001, 66, 0x4);
            EXPECT_EQ(
                describeClock(capped), "  clock 1785 MHz core over 1 reading, 9001 MHz memory, 66 °C — sw power cap\n");

            // A card at its own clock says so in words, rather than trailing an empty dash.
            const GpuClock free = GpuClock::reading(2325, 9001, 43, 0);
            EXPECT_EQ(describeClock(free),
                "  clock 2325 MHz core over 1 reading, 9001 MHz memory, 43 °C — nothing holding it back\n");
        }

        /// A place is described by every reading taken across its frames, and says how many.
        ///
        /// **A range of two is not a range.** The ends of a place agree to within a couple of per
        /// cent while the card moves a fifth of its clock between them, so a reader has to be able
        /// to tell a card that held still over many readings from one asked twice. The count is what
        /// says which, and the mean is the figure a frame time is read against.
        TEST(RtxGpuClockTest, everyReadingCountsTowardTheMeanAndTheCountIsPrinted)
        {
            GpuClock place = GpuClock::reading(1785, 9001, 61, 0x4);
            place.add(GpuClock::reading(2070, 9001, 66, 0x1));
            place.add(GpuClock::reading(1980, 9001, 64, 0x0));

            EXPECT_EQ(place.mLowestMhz, 1785u);
            EXPECT_EQ(place.mHighestMhz, 2070u);
            EXPECT_EQ(place.mReadings, 3u);

            // (1785 + 2070 + 1980) / 3 = 5835 / 3 = 1945.
            EXPECT_EQ(place.getMeanMhz(), 1945u);

            // The hottest it got, and every reason any reading saw — a card that went idle at one
            // point and was capped at another was both, and each says something about the numbers.
            EXPECT_EQ(place.mTemperatureC, 66u);
            EXPECT_EQ(describeClock(place),
                "  clock 1945 MHz core over 3 readings, 1785–2070, 9001 MHz memory, 66 °C — gpu idle, sw power cap\n");

            // **A reading nothing answered adds nothing**, so a sample that failed leaves the rest
            // standing rather than pulling the range down to zero or the mean toward it.
            const GpuClock held = place;
            place.add(GpuClock{});
            EXPECT_EQ(place.mLowestMhz, held.mLowestMhz);
            EXPECT_EQ(place.mHighestMhz, held.mHighestMhz);
            EXPECT_EQ(place.mReadings, held.mReadings);
            EXPECT_EQ(place.getMeanMhz(), held.getMeanMhz());

            // And the first reading into an empty one is that reading, rather than a range from
            // nought.
            GpuClock first;
            first.add(held);
            EXPECT_TRUE(first.mRead);
            EXPECT_EQ(first.mLowestMhz, held.mLowestMhz);
            EXPECT_EQ(first.mReadings, held.mReadings);

            // A card that never moved still says how many readings said so, which is what tells it
            // from one asked once.
            GpuClock still = GpuClock::reading(2325, 9001, 43, 0);
            still.add(GpuClock::reading(2325, 9001, 44, 0));
            EXPECT_EQ(describeClock(still),
                "  clock 2325 MHz core over 2 readings, 9001 MHz memory, 44 °C — nothing holding it back\n");
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

        /// A watch samples across the frames it is open for, and a second `start` does not disturb
        /// one already running.
        ///
        /// **The reading count is the whole claim.** What the row said before this existed was two
        /// samples printed as a range, and a reader could not tell that from a card watched
        /// throughout — so what a watch has to prove is that it took more than two.
        TEST(RtxGpuClockTest, aWatchSamplesAcrossThePlaceAndASecondStartLeavesItAlone)
        {
            if (!readGpuClock().mRead)
                GTEST_SKIP() << "nothing answered for a GPU clock on this machine";

            ClockWatch watch;
            watch.start();

            // Longer than the watch's own period, so the loop comes round at least once inside it
            // and the count can only be the sampling rather than the two ends.
            std::this_thread::sleep_for(std::chrono::milliseconds(700));

            // **A second start is nothing at all, forgetting included.** One of these is held across
            // the places of a suite, so a start that cleared a run in progress would throw away
            // every reading that run had taken.
            watch.start();

            const GpuClock place = watch.stop();
            EXPECT_TRUE(place.mRead);
            EXPECT_GT(place.mReadings, 2u) << "a watch that answered with no more than its two ends";
            EXPECT_GE(place.getMeanMhz(), place.mLowestMhz);
            EXPECT_LE(place.getMeanMhz(), place.mHighestMhz);

            // And it starts again from nothing, rather than carrying the place before it.
            watch.start();
            const GpuClock next = watch.stop();
            EXPECT_LT(next.mReadings, place.mReadings) << "the second place carried the first's readings";
        }
    }
}
