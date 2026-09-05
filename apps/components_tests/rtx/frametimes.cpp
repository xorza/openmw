#include <algorithm>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtx/frametimes.hpp>

namespace Rtx
{
    namespace
    {
        /// The six figures a run is quoted by, against hand-computed values.
        ///
        /// **Nearest rank, so every figure is a frame that happened.** The qth percentile is the
        /// `ceil(q * n)`th shortest counting from one, which is what makes these checkable by
        /// counting rather than by re-deriving whatever interpolation was chosen.
        TEST(RtxFrameTimesTest, everyFigureIsAFrameThatHappened)
        {
            // Ten frames, out of order so the sort is part of what is being tested. Sorted they run
            // 10, 11, 12, 13, 14, 15, 16, 17, 18, 30 and sum to 156.
            std::vector<double> ten{ 14.0, 12.0, 30.0, 10.0, 17.0, 13.0, 18.0, 11.0, 16.0, 15.0 };
            const FrameTimes small = summarise(ten);

            EXPECT_DOUBLE_EQ(small.mMean, 15.6) << "156 over ten";
            EXPECT_DOUBLE_EQ(small.mMedian, 14.0) << "ceil(0.5 x 10) = 5, and the fifth shortest is 14";
            EXPECT_DOUBLE_EQ(small.mP95, 30.0) << "ceil(0.95 x 10) = 10, which is the whole run";
            EXPECT_DOUBLE_EQ(small.mP99, 30.0);
            EXPECT_DOUBLE_EQ(small.mBest, 10.0);
            EXPECT_DOUBLE_EQ(small.mWorst, 30.0);

            EXPECT_TRUE(std::is_sorted(ten.begin(), ten.end())) << "summarising sorts what it was given";

            // A run long enough to tell the two tails apart, which is the argument for the ten
            // second default: at sixty frames p99 is the worst frame and says nothing.
            std::vector<double> hundred;
            hundred.reserve(100);
            for (int at = 100; at >= 1; --at)
                hundred.push_back(static_cast<double>(at));

            const FrameTimes large = summarise(hundred);

            EXPECT_DOUBLE_EQ(large.mMean, 50.5) << "1 through 100 averages to 50.5";
            EXPECT_DOUBLE_EQ(large.mMedian, 50.0) << "ceil(50) = 50";
            EXPECT_DOUBLE_EQ(large.mP95, 95.0);
            EXPECT_DOUBLE_EQ(large.mP99, 99.0);
            EXPECT_NE(large.mP95, large.mP99) << "a hundred frames separate the two tails";
            EXPECT_DOUBLE_EQ(large.mBest, 1.0);
            EXPECT_DOUBLE_EQ(large.mWorst, 100.0);

            // **The boundaries.** One frame is every figure at once, and two is the smallest run
            // where a nearest-rank median is the shorter of the pair rather than their average.
            std::vector<double> one{ 7.0 };
            const FrameTimes single = summarise(one);
            EXPECT_DOUBLE_EQ(single.mMean, 7.0);
            EXPECT_DOUBLE_EQ(single.mMedian, 7.0);
            EXPECT_DOUBLE_EQ(single.mP99, 7.0);
            EXPECT_DOUBLE_EQ(single.mWorst, 7.0);

            std::vector<double> two{ 9.0, 5.0 };
            const FrameTimes pair = summarise(two);
            EXPECT_DOUBLE_EQ(pair.mMean, 7.0);
            EXPECT_DOUBLE_EQ(pair.mMedian, 5.0) << "ceil(1) = 1, and the shorter of two is a frame that happened";
            EXPECT_DOUBLE_EQ(pair.mP95, 9.0);
        }

        /// The two rates, which are the median and the ninety-ninth percentile read as frame rates.
        TEST(RtxFrameTimesTest, aRateIsItsFrameTimeTheOtherWayUp)
        {
            // A twenty-millisecond median is fifty a second, and a fortieth of a second at the tail
            // is twenty-five: the pair a frame rate is normally quoted as.
            std::vector<double> times{ 20.0, 20.0, 20.0, 40.0 };
            const FrameTimes measured = summarise(times);

            EXPECT_DOUBLE_EQ(measured.mMedian, 20.0) << "ceil(2) = 2, the second of four";
            EXPECT_DOUBLE_EQ(measured.mP99, 40.0);
            EXPECT_DOUBLE_EQ(measured.getRate(), 50.0);
            EXPECT_DOUBLE_EQ(measured.getLowRate(), 25.0);
            EXPECT_LT(measured.getLowRate(), measured.getRate()) << "the tail is never the faster of the two";

            // A frame that took no time at all is a frame nobody measured, and dividing by it would
            // report infinity as a frame rate.
            EXPECT_DOUBLE_EQ(FrameTimes{}.getRate(), 0.0);
            EXPECT_DOUBLE_EQ(FrameTimes{}.getLowRate(), 0.0);
        }

        /// A zone is quoted by what it cost the run and not by what it cost a frame that ran it.
        ///
        /// **The row has to be summable**, which is what a pass that runs at a cell crossing broke:
        /// it was a median of the frames that crossed, and the row printed `micromap 7.51` above a
        /// frame median of 6.73 ms.
        TEST(RtxGpuBreakdownTest, aZoneIsQuotedOverTheWholeRun)
        {
            GpuBreakdown breakdown;
            EXPECT_TRUE(breakdown.empty());

            // Ten frames. `trace` runs in all of them at 4 ms, and `micromap` in the first two at
            // 20 ms and 10 ms — a pass that costs the run 30 ms and three of them per frame.
            const GpuSpan trace{ .mName = "trace", .mMs = 4.0 };
            for (int frame = 0; frame < 10; ++frame)
            {
                const GpuSpan micromap{ .mName = "micromap", .mMs = frame == 0 ? 20.0 : 10.0 };
                const std::vector<GpuSpan> spans
                    = frame < 2 ? std::vector<GpuSpan>{ micromap, trace } : std::vector<GpuSpan>{ trace };

                breakdown.add(spans);
            }

            const std::span<const GpuZone> zones = breakdown.summariseZones();
            ASSERT_EQ(zones.size(), 2u);

            EXPECT_EQ(zones[0].mName, "trace") << "4 ms of every frame beats 3 ms of the average one";
            EXPECT_DOUBLE_EQ(zones[0].mShareMs, 4.0) << "40 ms over ten frames";
            EXPECT_EQ(zones[0].mFrames, 10u);
            EXPECT_EQ(zones[0].mOfFrames, 10u);
            EXPECT_TRUE(zones[0].isEveryFrame());

            EXPECT_EQ(zones[1].mName, "micromap");
            EXPECT_DOUBLE_EQ(zones[1].mShareMs, 3.0) << "30 ms over ten frames, not the 10 ms it cost when it ran";
            EXPECT_EQ(zones[1].mFrames, 2u);
            EXPECT_EQ(zones[1].mOfFrames, 10u);
            EXPECT_FALSE(zones[1].isEveryFrame());
            EXPECT_DOUBLE_EQ(zones[1].mTimes.mMedian, 10.0) << "ceil(0.5 x 2) = 1, the shorter of the two";
            EXPECT_DOUBLE_EQ(zones[1].mTimes.mWorst, 20.0);

            // The device's part of the average frame is the row added up: 4 ms of trace and 3 ms
            // of micromap against the 70 ms it spent over ten frames.
            EXPECT_DOUBLE_EQ(zones[0].mShareMs + zones[1].mShareMs, 7.0);

            EXPECT_EQ(describeZone(zones[0]), "trace 4.00") << "a zone every frame ran needs no qualification";
            EXPECT_EQ(describeZone(zones[1]), "micromap 3.00 (10.00 on 2 of 10)");
            EXPECT_EQ(describeZones(zones), "  gpu ms    trace 4.00  micromap 3.00 (10.00 on 2 of 10)\n");
        }

        /// A pass recorded in batches opens its zone several times over one frame, and the frame is
        /// what it is charged to.
        ///
        /// **A row longer than the run is what this stops.** The structure builds are recorded per
        /// batch, so a crossing frame opened `tlas` twice — and the report then said the zone ran on
        /// 620 frames of a 601-frame run.
        TEST(RtxGpuBreakdownTest, aZoneOpenedTwiceInAFrameIsOneSampleOfIt)
        {
            GpuBreakdown breakdown;

            // Two frames. The first builds in two batches of 3 ms and 5 ms, the second in one of
            // 4 ms, and `trace` runs once in each.
            const std::vector<GpuSpan> batched{
                GpuSpan{ .mName = "tlas", .mMs = 3.0 },
                GpuSpan{ .mName = "trace", .mMs = 2.0 },
                GpuSpan{ .mName = "tlas", .mMs = 5.0 },
            };
            const std::vector<GpuSpan> once{
                GpuSpan{ .mName = "tlas", .mMs = 4.0 },
                GpuSpan{ .mName = "trace", .mMs = 2.0 },
            };

            breakdown.add(batched);
            breakdown.add(once);

            const std::span<const GpuZone> zones = breakdown.summariseZones();
            ASSERT_EQ(zones.size(), 2u);

            EXPECT_EQ(zones[0].mName, "tlas");
            EXPECT_EQ(zones[0].mFrames, 2u) << "two frames ran it, whatever the batches";
            EXPECT_EQ(zones[0].mOfFrames, 2u);
            EXPECT_DOUBLE_EQ(zones[0].mShareMs, 6.0) << "12 ms over two frames";
            EXPECT_DOUBLE_EQ(zones[0].mTimes.mWorst, 8.0) << "the frame that built twice cost the pair";
            EXPECT_DOUBLE_EQ(zones[0].mTimes.mBest, 4.0);

            EXPECT_EQ(zones[1].mName, "trace");
            EXPECT_DOUBLE_EQ(zones[1].mShareMs, 2.0);
        }

        /// A frame that reported no zone at all still counts against every share.
        TEST(RtxGpuBreakdownTest, aFrameWithNoZonesIsStillAFrame)
        {
            GpuBreakdown breakdown;

            const GpuSpan trace{ .mName = "trace", .mMs = 6.0 };
            const std::vector<GpuSpan> one{ trace };
            breakdown.add(one);

            // Three more frames the device wrote no timestamp for, which is what the first frames
            // of a run look like.
            for (int frame = 0; frame < 3; ++frame)
                breakdown.add({});

            const std::span<const GpuZone> zones = breakdown.summariseZones();
            ASSERT_EQ(zones.size(), 1u);
            EXPECT_DOUBLE_EQ(zones[0].mShareMs, 1.5) << "6 ms over the four frames measured";
            EXPECT_EQ(zones[0].mOfFrames, 4u);
            EXPECT_EQ(describeZones({}), "") << "a run with no zones prints no row at all";
        }
    }
}
