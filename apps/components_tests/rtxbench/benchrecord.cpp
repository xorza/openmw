#include <string>

#include <gtest/gtest.h>

#include <components/rtx/framespend.hpp>
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

        /// The arrivals count the frames that extended the scene and keep the three worst frames of
        /// the run whatever they carried, longest first, so a reader can say whose the tail is.
        TEST(RtxBenchRecordTest, arrivalsCountExtendingFramesAndKeepTheThreeWorst)
        {
            Arrivals arrivals;
            FrameSpend spend;
            spend.at(Timing::Place) = 2.0;
            spend.at(Timing::Upload) = 1.9;
            spend.at(Timing::Finish) = 1.5;
            spend.at(Timing::Wait) = 1.4;
            spend.at(Timing::Walk) = 0.5;
            // The driver's sleep is the largest figure of the frame and a share of `update`, so it
            // is not among the stretches printed.
            spend.at(Timing::Update) = 4.0;
            spend.at(Timing::Sleep) = 3.5;

            const auto frameOf = [&](const double frameMs) {
                FrameSpend frame = spend;
                frame.at(Timing::Frame) = frameMs;
                return frame;
            };
            arrivals.add(0, frameOf(5.0));
            arrivals.add(12, frameOf(9.0));
            arrivals.add(0, frameOf(4.0));
            arrivals.add(3, frameOf(7.0));
            arrivals.add(0, frameOf(6.0));

            EXPECT_EQ(arrivals.mFrames, 2u);
            EXPECT_EQ(arrivals.mMeshes, 15u);
            EXPECT_EQ(arrivals.mWorstMs, 9.0);
            EXPECT_EQ(arrivals.getMeanMs(), 8.0) << "(9 + 7) / 2";

            ASSERT_EQ(arrivals.mWorstCount, 3u);
            EXPECT_EQ(arrivals.mWorst[0].getFrameMs(), 9.0);
            EXPECT_EQ(arrivals.mWorst[0].mArrivedMeshes, 12u);
            EXPECT_EQ(arrivals.mWorst[1].getFrameMs(), 7.0);
            EXPECT_EQ(arrivals.mWorst[2].getFrameMs(), 6.0);
            EXPECT_EQ(arrivals.mWorst[2].mArrivedMeshes, 0u);

            BenchPlace place;
            place.mView = "still";
            place.mFrames = 5;
            place.mWallSeconds = 1.0;
            place.mArrivals = arrivals;
            const std::string described = describePlace(place);
            EXPECT_NE(described.find("2 frames extended the scene with 15 meshes"), std::string::npos) << described;
            // The three largest stretches, and never a share beside its whole: `upload` is most of
            // `place`, `wait` most of `finish` and `sleep` most of `update`, so `update` leads and
            // `finish` is the third.
            EXPECT_NE(described.find("9.0 ms (12 meshes: update 4.0 place 2.0 finish 1.5)"), std::string::npos)
                << described;

            // The driver's own latency stands under the rows only where the driver paced the
            // window, and the JSON leaves the key out otherwise.
            EXPECT_EQ(described.find("latency ms"), std::string::npos) << described;
            place.mLatency = FrameTimes{
                .mMean = 12.0, .mMedian = 11.5, .mP95 = 14.0, .mP99 = 15.0, .mBest = 9.0, .mWorst = 20.0
            };
            EXPECT_NE(describePlace(place).find("latency ms"), std::string::npos) << describePlace(place);
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

        /// A place whose textures stand smaller than their files says how many on the scene's line,
        /// and one drawn as its files are says nothing about it.
        TEST(RtxBenchRecordTest, thePlaceSaysHowManyTexturesStandSmallerOnlyWhereAnyDo)
        {
            BenchPlace place;
            place.mView = "town";
            place.mCell = "-3,-2";
            place.mFrames = 1;
            place.mWallSeconds = 1.0;
            place.mScene.mTextureCount = 737;

            EXPECT_EQ(describePlace(place).find("smaller"), std::string::npos) << describePlace(place);

            place.mScene.mReducedTextureCount = 212;
            EXPECT_NE(describePlace(place).find("737 textures, 0.0 MiB, 212 of them held smaller"), std::string::npos)
                << describePlace(place);
        }
    }
}
