#include <algorithm>
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
    }
}
