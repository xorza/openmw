#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtxbench/benchspec.hpp>

namespace Rtx
{
    namespace
    {
        TEST(RtxBenchSpecTest, aListOfNamesLosesItsSpacingAndItsEmptyEntries)
        {
            EXPECT_EQ(splitNames("balmora,vivec"), (std::vector<std::string>{ "balmora", "vivec" }));
            EXPECT_EQ(splitNames("  balmora ,\tvivec  "), (std::vector<std::string>{ "balmora", "vivec" }));

            // A trailing comma is what a list being edited looks like halfway through, and an empty
            // entry is not a view whose name is the empty string.
            EXPECT_EQ(splitNames("balmora,,vivec,"), (std::vector<std::string>{ "balmora", "vivec" }));
            EXPECT_EQ(splitNames("one"), (std::vector<std::string>{ "one" }));
            EXPECT_TRUE(splitNames("").empty());
            EXPECT_TRUE(splitNames("  ,  ").empty());
        }

        /// A span is frames or seconds, and which one it is decides how it is counted.
        TEST(RtxBenchSpecTest, aSpanIsFramesOrSecondsAndCountsInFramesEitherWay)
        {
            EXPECT_EQ((BenchSpan{ .mFrames = 240 }).getFrames(), 240u);

            // Ten seconds of world at sixty frames a second is six hundred frames, on every machine
            // and on every build. That is the whole of why a run is written in seconds of world.
            EXPECT_EQ((BenchSpan{ .mSeconds = 10.0f }).getFrames(), 600u);

            // A span short enough to round to nothing still measures the frame it asked for.
            EXPECT_EQ((BenchSpan{ .mSeconds = 0.001f }).getFrames(), 1u);

            // Nothing asked for is nothing measured, which is what an absent warm-up says.
            EXPECT_TRUE(BenchSpan{}.empty());
            EXPECT_EQ(BenchSpan{}.getFrames(), 0u);

            // A window's run is a count like any other to the schedule, and its own thing to
            // whatever would make room for it.
            EXPECT_TRUE((BenchSpan{ .mFrames = BenchSpan::sUntilClosed }).isUntilClosed());
            EXPECT_EQ((BenchSpan{ .mFrames = BenchSpan::sUntilClosed }).getFrames(), BenchSpan::sUntilClosed);
            EXPECT_FALSE((BenchSpan{ .mFrames = 240 }).isUntilClosed());
            EXPECT_FALSE((BenchSpan{ .mSeconds = 10.0f }).isUntilClosed());

            const BenchSpec spec{ .mRun = { .mSeconds = 10.0f }, .mWarm = { .mFrames = 120 } };
            EXPECT_EQ(spec.getMeasured(), 600u);
            EXPECT_EQ(spec.getWarmup(), 120u);
        }
    }
}
