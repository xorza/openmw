#include <algorithm>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtx/framespend.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtxbench/frametimes.hpp>
#include <components/testing/util.hpp>

#include "../rtx/allocations.hpp"

namespace Rtx
{
    namespace
    {
        /// A line closes on the frame that fills a second, describes exactly that second, and the
        /// next second starts from nothing.
        TEST(RtxFrameRateTest, aSecondOfFramesClosesOneLineAndTheNextStartsFromNothing)
        {
            FrameRate rate;
            EXPECT_TRUE(rate.getText().empty()) << "nothing has closed";

            // Ninety-nine frames of ten milliseconds are 990 ms, which is short of a second.
            for (int at = 0; at < 99; ++at)
                EXPECT_FALSE(rate.add(10.0)) << "frame " << at + 1 << " closed a line early";
            EXPECT_TRUE(rate.getText().empty()) << "an open second has no line";

            const std::size_t before = Testing::getAllocationCount();
            const bool closed = rate.add(10.0);
            const std::size_t spent = Testing::getAllocationCount() - before;

            EXPECT_TRUE(closed) << "the hundredth frame is the second";
            EXPECT_EQ(spent, 0u) << "closing a line reached the heap " << spent << " times";
            EXPECT_EQ(rate.getText(), "100 fps, 10.0 ms, worst 10.0 ms");

            // Forty frames alternating 5 and 45 ms sum to 20 x 5 + 20 x 45 = 1000 exactly, with a
            // mean of 25 — so the worst is the figure the mean hides. The first thirty-nine are
            // 20 x 5 + 19 x 45 = 955, which is still open.
            for (int at = 0; at < 39; ++at)
                EXPECT_FALSE(rate.add(at % 2 == 0 ? 5.0 : 45.0)) << "frame " << at + 1 << " of the second second";
            EXPECT_EQ(rate.getText(), "100 fps, 10.0 ms, worst 10.0 ms") << "an open second leaves the line alone";

            EXPECT_TRUE(rate.add(45.0));
            EXPECT_EQ(rate.getText(), "40 fps, 25.0 ms, worst 45.0 ms")
                << "the first second's worst did not carry over";

            // A rate that does not divide a second: 117 x 8.5 = 994.5 is open and 118 x 8.5 = 1003
            // closes, at 1000 / 8.5 = 117.6 frames a second rounded to the nearest whole one.
            for (int at = 0; at < 117; ++at)
                EXPECT_FALSE(rate.add(8.5));
            EXPECT_TRUE(rate.add(8.5));
            EXPECT_EQ(rate.getText(), "118 fps, 8.5 ms, worst 8.5 ms");

            // One frame longer than a second is a second on its own: 1000 / 1500 rounds to one.
            EXPECT_TRUE(rate.add(1500.0));
            EXPECT_EQ(rate.getText(), "1 fps, 1500.0 ms, worst 1500.0 ms");
        }
    }

    // What a fifo carries is `perffifo.cpp`, built where there is one; these two need none.
    namespace
    {
        TEST(RtxPerfControlTest, aRunThatIsNotBeingProfiledSaysNothingAndOpensNothing)
        {
            PerfControl control(std::filesystem::path{});
            control.enable();
            control.disable();

            // The point of the empty path: `bench` holds one of these whether or not perf is there,
            // and the one that is not connected to anything must not go looking for a fifo.
            SUCCEED();
        }

        /// On Linux, a path nothing reads; on Windows, any path at all. Named either way.
        TEST(RtxPerfControlTest, aFifoWithNobodyReadingItIsNamedRatherThanWaitedOn)
        {
            const std::filesystem::path missing = TestingOpenMW::outputFilePath("perf-control-absent");
            std::filesystem::remove(missing);

            PerfControl control(missing);
            EXPECT_THROW(control.enable(), std::system_error);
        }
    }

    namespace
    {
        /// The rows are read across each other a frame at a time, and the series is written the
        /// same way: one line a frame, every row a column, the wait among them.
        TEST(RtxFrameSamplesTest, everyRowIsOneSampleLongerAFrameAndTheSeriesIsWrittenAcross)
        {
            FrameSamples samples;
            EXPECT_TRUE(samples.empty());

            // Two frames. The first waited 4.5 ms of its 10; the second, with nothing in flight
            // to wait for, waited nought and was held 0.75 ms by the driver before its input.
            FrameSpend first;
            first.at(Timing::Frame) = 10.0;
            first.at(Timing::Wait) = 4.5;
            first.at(Timing::Finish) = 4.75;
            first.at(Timing::Trace) = 1.25;
            samples.add(first);

            FrameSpend second;
            second.at(Timing::Frame) = 8.0;
            second.at(Timing::Trace) = 1.5;
            second.at(Timing::Update) = 2.0;
            second.at(Timing::Sleep) = 0.75;
            samples.add(second);

            EXPECT_EQ(samples.size(), 2u);
            for (const Timing timing : sTimings.values())
                EXPECT_EQ(samples.at(timing).size(), 2u) << sTimings.name(timing) << " is out of step";
            EXPECT_DOUBLE_EQ(samples.at(Timing::Wait)[0], 4.5);
            EXPECT_DOUBLE_EQ(samples.at(Timing::Wait)[1], 0.0);
            EXPECT_DOUBLE_EQ(samples.at(Timing::Frame)[1], 8.0);

            const std::filesystem::path file = TestingOpenMW::outputFilePath("frame-times.txt");
            writeFrameTimes(file, samples);

            std::ifstream written(file);
            std::string text((std::istreambuf_iterator<char>(written)), std::istreambuf_iterator<char>());
            EXPECT_EQ(text,
                "frame finish wait walk fold place bake textures upload trace views present update sleep\n"
                "10.000 4.750 4.500 0.000 0.000 0.000 0.000 0.000 0.000 1.250 0.000 0.000 0.000 0.000\n"
                "8.000 0.000 0.000 0.000 0.000 0.000 0.000 0.000 0.000 1.500 0.000 0.000 2.000 0.750\n");

            // A directory nobody made is named rather than written past.
            EXPECT_THROW(writeFrameTimes(TestingOpenMW::outputFilePath("no-such-dir") / "frame-times.txt", samples),
                std::exception);

            samples.clear();
            EXPECT_TRUE(samples.empty());
            EXPECT_TRUE(samples.at(Timing::Wait).empty());
        }

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
        /// it was a median of the frames that crossed, and the row printed `blas 7.51` above a
        /// frame median of 6.73 ms.
        TEST(RtxGpuBreakdownTest, aZoneIsQuotedOverTheWholeRun)
        {
            GpuBreakdown breakdown;
            EXPECT_TRUE(breakdown.empty());

            // Ten frames. `trace` runs in all of them at 4 ms, and `blas` in the first two at
            // 20 ms and 10 ms — a pass that costs the run 30 ms and three of them per frame.
            const GpuSpan trace{ .mName = "trace", .mMs = 4.0 };
            for (int frame = 0; frame < 10; ++frame)
            {
                const GpuSpan blas{ .mName = "blas", .mMs = frame == 0 ? 20.0 : 10.0 };
                const std::vector<GpuSpan> spans
                    = frame < 2 ? std::vector<GpuSpan>{ blas, trace } : std::vector<GpuSpan>{ trace };

                breakdown.add(spans);
            }

            const std::span<const GpuZone> zones = breakdown.summariseZones();
            ASSERT_EQ(zones.size(), 2u);

            EXPECT_EQ(zones[0].mName, "trace") << "4 ms of every frame beats 3 ms of the average one";
            EXPECT_DOUBLE_EQ(zones[0].mShareMs, 4.0) << "40 ms over ten frames";
            EXPECT_EQ(zones[0].mFrames, 10u);
            EXPECT_EQ(zones[0].mOfFrames, 10u);
            EXPECT_TRUE(zones[0].isEveryFrame());

            EXPECT_EQ(zones[1].mName, "blas");
            EXPECT_DOUBLE_EQ(zones[1].mShareMs, 3.0) << "30 ms over ten frames, not the 10 ms it cost when it ran";
            EXPECT_EQ(zones[1].mFrames, 2u);
            EXPECT_EQ(zones[1].mOfFrames, 10u);
            EXPECT_FALSE(zones[1].isEveryFrame());
            EXPECT_DOUBLE_EQ(zones[1].mTimes.mMedian, 10.0) << "ceil(0.5 x 2) = 1, the shorter of the two";
            EXPECT_DOUBLE_EQ(zones[1].mTimes.mWorst, 20.0);

            // The device's part of the average frame is the row added up: 4 ms of trace and 3 ms
            // of blas against the 70 ms it spent over ten frames.
            EXPECT_DOUBLE_EQ(zones[0].mShareMs + zones[1].mShareMs, 7.0);

            EXPECT_EQ(describeZone(zones[0]), "trace 4.00") << "a zone every frame ran needs no qualification";
            EXPECT_EQ(describeZone(zones[1]), "blas 3.00 (10.00 on 2 of 10)");
            EXPECT_EQ(describeZones(zones), "  gpu ms    trace 4.00  blas 3.00 (10.00 on 2 of 10)\n");

            // **The next stop keeps the rows and quotes only what it met.** Emptied, a stop that
            // ran `trace` alone reports `trace` alone, over its own ten frames and out of the room
            // the rows were made: a row a zone outgrew mid-run grew inside a frame it was timing.
            breakdown.reserve(16);
            breakdown.clear();
            EXPECT_TRUE(breakdown.empty());

            const std::size_t before = Testing::getAllocationCount();
            for (int frame = 0; frame < 10; ++frame)
                breakdown.add(std::span<const GpuSpan>(&trace, 1));
            const std::span<const GpuZone> again = breakdown.summariseZones();
            EXPECT_EQ(Testing::getAllocationCount() - before, 0u) << "a second stop grew a row or the summary";

            ASSERT_EQ(again.size(), 1u) << "a zone the first stop met and this one did not is not quoted";
            EXPECT_EQ(again[0].mName, "trace");
            EXPECT_DOUBLE_EQ(again[0].mShareMs, 4.0);
            EXPECT_EQ(again[0].mOfFrames, 10u);
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
