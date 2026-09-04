#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

#include <components/rtx/spritelistsize.hpp>

namespace Rtx
{
    namespace
    {
        /// A frame's tiles at 1920x1080, which is 120 by 68 at a sixteen-pixel tile.
        constexpr std::uint32_t sTiles = 120 * 68;

        /// The buffer holds the starts and the capacity, and the pass is told the capacity alone.
        ///
        /// **The two halves of one number, and a test that they stay that.** A buffer sized from one
        /// figure and a capacity taken from another is a dispatch past the end of an allocation, and
        /// the two used to be three statements apart.
        TEST(RtxSpriteListSizeTest, theBufferHoldsTheStartsAndTheCapacityTogether)
        {
            SpriteListSize size;
            size.sizeFor(sTiles, 4096, 0);

            EXPECT_EQ(size.getEntries(), std::uint64_t{ sTiles } + 1 + size.getCapacity());
            EXPECT_EQ(size.getBytes(), size.getEntries() * sizeof(std::uint32_t));
        }

        /// The floor is a share of the tiles, the report doubles it, and neither ever gives ground.
        ///
        /// **Hand-computed.** 4,096 sprites over 8,160 tiles is 33,423,360, and one tile in
        /// `sFloorShare` of that is 522,240 — which is what a copy nothing has reported to gets. A
        /// report of 600,000 then asks for twice that, and a report of one asks for less than the
        /// mark already holds and moves nothing.
        TEST(RtxSpriteListSizeTest, theMarkFollowsTheFloorAndTheReportAndNeverGoesBack)
        {
            constexpr std::uint32_t sSprites = 4096;
            constexpr std::uint32_t sFloor
                = static_cast<std::uint32_t>(std::uint64_t{ sSprites } * sTiles / SpriteListSize::sFloorShare);
            static_assert(sFloor == 522240, "the floor is not what the case says it is");

            SpriteListSize size;
            EXPECT_EQ(size.getCapacity(), 0u) << "a copy nothing has sized";

            size.sizeFor(sTiles, sSprites, 0);
            EXPECT_EQ(size.getCapacity(), sFloor) << "the first frame takes the floor";

            size.sizeFor(sTiles, sSprites, 600000);
            EXPECT_EQ(size.getCapacity(), 1200000u) << "a report is doubled";

            size.sizeFor(sTiles, sSprites, 1);
            EXPECT_EQ(size.getCapacity(), 1200000u) << "a quiet frame gave the room back";

            size.sizeFor(sTiles, 0, 0);
            EXPECT_EQ(size.getCapacity(), 1200000u) << "a frame with no sprites gave the room back";
        }

        /// Neither the floor nor the report can carry the mark past the cap or round the top.
        ///
        /// **The whole reason the mark is capped and the arithmetic is sixty-four bits wide.** A
        /// share of the tiles is a product of two counts and twice a report is a doubling of a
        /// number the device wrote, and in thirty-two bits each of them wraps to something smaller —
        /// which would size the buffer under a capacity that did not shrink with it. Past the cap a
        /// frame is drawn by walking every sprite, which is slow and right.
        TEST(RtxSpriteListSizeTest, nothingCarriesTheMarkPastTheCapOrRoundTheTop)
        {
            constexpr std::uint32_t sMost = std::numeric_limits<std::uint32_t>::max();

            // 4,000,000 sprites over 8,160 tiles is 32,640,000,000, which is past a `uint32` before
            // the share is taken and 510,000,000 after it — itself past the cap.
            SpriteListSize byFloor;
            byFloor.sizeFor(sTiles, 4000000, 0);
            EXPECT_EQ(byFloor.getCapacity(), SpriteListSize::sMostEntries) << "the floor went round";

            // Doubling this wraps to 2,147,483,646 in thirty-two bits, which is smaller than the
            // report itself.
            SpriteListSize byReport;
            byReport.sizeFor(sTiles, 0, 0x8000'0000u + 1u);
            EXPECT_EQ(byReport.getCapacity(), SpriteListSize::sMostEntries) << "the doubling went round";

            // And the widest of everything at once, which is what says the sum below cannot wrap.
            SpriteListSize byBoth;
            byBoth.sizeFor(sMost, sMost, sMost);
            EXPECT_EQ(byBoth.getCapacity(), SpriteListSize::sMostEntries);
            EXPECT_EQ(byBoth.getEntries(), std::uint64_t{ sMost } + 1 + SpriteListSize::sMostEntries)
                << "the length is the starts and the capacity, in sixty-four bits";
        }
    }
}
