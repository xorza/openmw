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
            const std::optional<BenchSpan> frames = readSpan("240");
            ASSERT_TRUE(frames.has_value());
            EXPECT_EQ(frames->mFrames, 240u);
            EXPECT_EQ(frames->mSeconds, 0.0f);
            EXPECT_EQ(frames->getFrames(), 240u);
            EXPECT_EQ(frames->describe(), "240 frames");

            // Ten seconds of world at sixty frames a second is six hundred frames, on every machine
            // and on every build. That is the whole of why a run is written in seconds of world.
            const std::optional<BenchSpan> seconds = readSpan("10s");
            ASSERT_TRUE(seconds.has_value());
            EXPECT_EQ(seconds->mFrames, 0u);
            EXPECT_EQ(seconds->mSeconds, 10.0f);
            EXPECT_EQ(seconds->getFrames(), 600u);
            EXPECT_EQ(seconds->describe(), "10 s");

            // Nothing asked for is nothing measured, which is what an absent warm-up says.
            EXPECT_TRUE(BenchSpan{}.empty());
            EXPECT_EQ(BenchSpan{}.getFrames(), 0u);

            EXPECT_FALSE(readSpan("").has_value());
            EXPECT_FALSE(readSpan("s").has_value());
            EXPECT_FALSE(readSpan("ten").has_value());
            EXPECT_FALSE(readSpan("10x").has_value());
            EXPECT_FALSE(readSpan("10 s").has_value());
        }

        /// The four spellings a run's length is written in, and the one parser both hosts read.
        TEST(RtxBenchSpecTest, aRunIsALengthAWarmupAndASpeed)
        {
            std::string complaint;

            const std::optional<BenchSpec> plain = readSpec("600", complaint);
            ASSERT_TRUE(plain.has_value()) << complaint;
            EXPECT_EQ(plain->getMeasured(), 600u);
            EXPECT_EQ(plain->getWarmup(), 0u) << "a run that named no warm-up warms up for nothing";
            EXPECT_EQ(plain->mSpeed, 0.0f) << "a run that named no speed stands still";

            const std::optional<BenchSpec> warmed = readSpec("10s:2s", complaint);
            ASSERT_TRUE(warmed.has_value()) << complaint;
            EXPECT_EQ(warmed->getMeasured(), 600u);
            EXPECT_EQ(warmed->getWarmup(), 120u);

            // The speed comes off the end first, so the run and its warm-up need not know about it.
            const std::optional<BenchSpec> flown = readSpec("10s:2s@12000", complaint);
            ASSERT_TRUE(flown.has_value()) << complaint;
            EXPECT_EQ(flown->getMeasured(), 600u);
            EXPECT_EQ(flown->getWarmup(), 120u);
            EXPECT_EQ(flown->mSpeed, 12000.0f);

            // The two halves may be spelled differently, since a warm-up is about the card and a
            // run is about the world.
            const std::optional<BenchSpec> mixed = readSpec("600:120", complaint);
            ASSERT_TRUE(mixed.has_value()) << complaint;
            EXPECT_EQ(mixed->getMeasured(), 600u);
            EXPECT_EQ(mixed->getWarmup(), 120u);
        }

        /// **A run nobody can read the settings of is not a run.** Every refusal names what it could
        /// not read, because the alternative is a number measured over a length nobody asked for.
        TEST(RtxBenchSpecTest, aSpecThatWillNotParseIsRefusedAndSaysWhy)
        {
            std::string complaint;

            EXPECT_FALSE(readSpec("", complaint).has_value());
            EXPECT_FALSE(complaint.empty());

            EXPECT_FALSE(readSpec("0", complaint).has_value()) << "a run of no frames measures nothing";
            EXPECT_FALSE(readSpec("ten seconds", complaint).has_value());
            EXPECT_FALSE(readSpec("10s:", complaint).has_value()) << "a warm-up that was started and not written";
            EXPECT_FALSE(readSpec("10s@", complaint).has_value()) << "a speed that was started and not written";
            EXPECT_FALSE(readSpec("10s@0", complaint).has_value()) << "a route flown at no speed is not a route";
            EXPECT_FALSE(readSpec("10s@-5", complaint).has_value());
        }
    }
}
