#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include <apps/rtxtool/framehashes.hpp>

namespace RtxTool
{
    namespace
    {
        /// A run of `count` frames of one place whose pixels are the frame index, except the frames
        /// named in `moved`, which carry another value.
        FrameHashes runOf(const std::uint32_t count, const std::span<const std::uint32_t> moved)
        {
            FrameHashes hashes;
            for (std::uint32_t frame = 0; frame < count; ++frame)
            {
                const bool differs = std::ranges::find(moved, frame) != moved.end();
                const std::vector<std::uint8_t> pixels(4, static_cast<std::uint8_t>(differs ? 200 + frame : frame));
                hashes.add("place", frame, pixels);
            }

            return hashes;
        }

        /// **A frame after a mid-run build is named and not judged; a frame before it is both.**
        TEST(RtxFrameHashesTest, aBuildMidRunBoundsWhatARunIsHeldTo)
        {
            const FrameHashes reference = runOf(10, {});
            EXPECT_TRUE(runOf(10, {}).against(reference)[0].holds());

            // Frame seven differs, and the build was at five: named, and the run holds.
            FrameHashes after = runOf(10, std::array{ 7u });
            after.noteBuild("place", 5);
            const std::vector<FrameHashes::ViewDifference> onlyAfter = after.against(reference);
            ASSERT_EQ(onlyAfter.size(), 1u);
            EXPECT_EQ(onlyAfter[0].mDiffering, (std::vector<std::uint32_t>{ 7 }));
            EXPECT_EQ(onlyAfter[0].mBuiltAt, 5u);
            EXPECT_FALSE(onlyAfter[0].same());
            EXPECT_TRUE(onlyAfter[0].holds());
            EXPECT_EQ(describe(onlyAfter[0]),
                "1 of 10 frames differ, at 7, every one after the build at frame 5, which the driver's settling can "
                "do");

            // Frame three differs as well, which is before the build: the run does not hold. The
            // later build noted adds nothing.
            FrameHashes before = runOf(10, std::array{ 3u, 7u });
            before.noteBuild("place", 5);
            before.noteBuild("place", 8);
            const std::vector<FrameHashes::ViewDifference> both = before.against(reference);
            EXPECT_EQ(both[0].mBuiltAt, 5u);
            EXPECT_FALSE(both[0].holds());
            EXPECT_EQ(describe(both[0]), "2 of 10 frames differ, at 3, 7, 1 of them before the build at frame 5");

            // With no build noted, every frame is judged.
            const FrameHashes unbuilt = runOf(10, std::array{ 7u });
            EXPECT_FALSE(unbuilt.against(reference)[0].mBuiltAt.has_value());
            EXPECT_FALSE(unbuilt.against(reference)[0].holds());
        }
    }
}
