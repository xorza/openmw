#include <string>

#include <gtest/gtest.h>

#include <components/rtxbench/benchrecord.hpp>

namespace Rtx
{
    namespace
    {
        /// A crossing is the frame that dropped, whole: the count, how many rebuilt, the worst and
        /// the total are what the report reads, and nothing else is measured.
        TEST(RtxBenchRecordTest, crossingsCountTheFramesThatDropped)
        {
            Crossings crossings;
            crossings.add(false, 40.0);
            crossings.add(true, 250.0);
            crossings.add(false, 10.0);

            EXPECT_EQ(crossings.mCount, 3u);
            EXPECT_EQ(crossings.mRebuilds, 1u);
            EXPECT_EQ(crossings.mWorstMs, 250.0);
            EXPECT_EQ(crossings.mTotalMs, 300.0);
        }

        /// A route flown short says so beside its crossings, and one that arrived says nothing.
        TEST(RtxBenchRecordTest, thePlaceSaysHowMuchOfARouteWasFlownOnlyWhereItEndedShort)
        {
            BenchPlace place;
            place.mView = "route";
            place.mFrames = 1;
            place.mWallSeconds = 1.0;
            place.mCrossings.add(true, 100.0);

            EXPECT_EQ(describePlace(place).find("of the route flown"), std::string::npos);

            place.mTravelled = 0.25;
            EXPECT_NE(describePlace(place).find("25% of the route flown"), std::string::npos);
        }
    }
}
