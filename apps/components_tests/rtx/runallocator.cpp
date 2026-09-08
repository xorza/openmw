#include <gtest/gtest.h>

#include <components/rtx/runallocator.hpp>

namespace Rtx
{
    namespace
    {
        /// Runs handed out one after another are laid end to end, and the buffer is as long as they
        /// are together.
        TEST(RtxRunAllocatorTest, runsAreLaidEndToEndAndTheBufferIsAsLongAsTheySum)
        {
            RunAllocator allocator;

            EXPECT_EQ(allocator.allocate(4), (Rtx::Run{ .mOffset = 0, .mCount = 4 }));
            EXPECT_EQ(allocator.allocate(1), (Rtx::Run{ .mOffset = 4, .mCount = 1 }));
            EXPECT_EQ(allocator.allocate(7), (Rtx::Run{ .mOffset = 5, .mCount = 7 }));

            EXPECT_EQ(allocator.getEnd(), 12u);
            EXPECT_EQ(allocator.getFree(), 0u);
            EXPECT_EQ(allocator.getHoleCount(), 0u);
        }

        /// A hole is filled by the run that fits it worst-but-still, not by the first one offered.
        ///
        /// Two holes of four and ten, and a run of four: first fit would take the ten and leave a
        /// four-long hole that the next four-long run has to append past, so the buffer grows.
        /// Best fit takes the four, and the ten is still there for something ten long.
        TEST(RtxRunAllocatorTest, aRunGoesInTheSmallestHoleThatHoldsIt)
        {
            RunAllocator allocator;

            const Rtx::Run first = allocator.allocate(10);
            allocator.allocate(1);
            const Rtx::Run small = allocator.allocate(4);
            allocator.allocate(1);
            ASSERT_EQ(allocator.getEnd(), 16u);

            allocator.release(first);
            allocator.release(small);
            ASSERT_EQ(allocator.getHoleCount(), 2u);

            // The four-long hole, which is at eleven: ten, one, then four.
            EXPECT_EQ(allocator.allocate(4), (Rtx::Run{ .mOffset = 11, .mCount = 4 }));

            // And the ten is intact, so nothing had to be appended.
            EXPECT_EQ(allocator.allocate(10), (Rtx::Run{ .mOffset = 0, .mCount = 10 }));
            EXPECT_EQ(allocator.getEnd(), 16u);
        }

        /// A run that does not fill its hole leaves the rest of it behind.
        TEST(RtxRunAllocatorTest, aRunSmallerThanItsHoleLeavesTheRemainder)
        {
            RunAllocator allocator;

            const Rtx::Run wide = allocator.allocate(10);
            allocator.allocate(1);
            allocator.release(wide);

            EXPECT_EQ(allocator.allocate(3), (Rtx::Run{ .mOffset = 0, .mCount = 3 }));
            EXPECT_EQ(allocator.getFree(), 7u);
            EXPECT_EQ(allocator.getHoleCount(), 1u);

            // Three and the one above the hole, against a reach of eleven with seven of it free.
            EXPECT_EQ(allocator.getEnd(), 11u);
            EXPECT_EQ(allocator.getUsed(), 4u);

            EXPECT_EQ(allocator.allocate(7), (Rtx::Run{ .mOffset = 3, .mCount = 7 }));
            EXPECT_EQ(allocator.getFree(), 0u);
            EXPECT_EQ(allocator.getUsed(), 11u) << "a hole filled leaves nothing between the runs";
            EXPECT_EQ(allocator.getEnd(), 11u);
        }

        /// Runs given back beside one another become one hole, whichever order they come back in.
        ///
        /// **This is what a cell leaving is.** It arrived as thousands of small runs laid end to
        /// end and it leaves as thousands of releases; without merging, the next cell would find
        /// thousands of holes none of which is big enough for anything, and would append past all
        /// of them.
        TEST(RtxRunAllocatorTest, releasesThatTouchBecomeOneHole)
        {
            for (const bool backwards : { false, true })
            {
                RunAllocator allocator;

                const Rtx::Run a = allocator.allocate(3);
                const Rtx::Run b = allocator.allocate(3);
                const Rtx::Run c = allocator.allocate(3);
                allocator.allocate(1);
                ASSERT_EQ(allocator.getEnd(), 10u);

                // The middle one first either way, so the merge is exercised on both sides.
                allocator.release(b);
                if (backwards)
                {
                    allocator.release(c);
                    allocator.release(a);
                }
                else
                {
                    allocator.release(a);
                    allocator.release(c);
                }

                EXPECT_EQ(allocator.getHoleCount(), 1u) << "backwards: " << backwards;
                EXPECT_EQ(allocator.getFree(), 9u) << "backwards: " << backwards;
                EXPECT_EQ(allocator.allocate(9), (Rtx::Run{ .mOffset = 0, .mCount = 9 })) << "backwards: " << backwards;
            }
        }

        /// A hole that reaches the end of the buffer shortens the buffer instead of staying a hole.
        TEST(RtxRunAllocatorTest, aHoleAtTheEndGivesTheRoomBack)
        {
            RunAllocator allocator;

            allocator.allocate(5);
            const Rtx::Run last = allocator.allocate(6);
            ASSERT_EQ(allocator.getEnd(), 11u);

            allocator.release(last);

            EXPECT_EQ(allocator.getEnd(), 5u) << "the buffer is as long as what is in it";
            EXPECT_EQ(allocator.getHoleCount(), 0u);
            EXPECT_EQ(allocator.getFree(), 0u);
        }

        /// The run before the end goes with it: merging first, shrinking second.
        TEST(RtxRunAllocatorTest, aHoleMergedIntoTheEndGivesBothBack)
        {
            RunAllocator allocator;

            allocator.allocate(2);
            const Rtx::Run middle = allocator.allocate(3);
            const Rtx::Run last = allocator.allocate(4);

            allocator.release(middle);
            EXPECT_EQ(allocator.getEnd(), 9u) << "still held up by the last run";

            allocator.release(last);
            EXPECT_EQ(allocator.getEnd(), 2u) << "and now nothing holds it up";
            EXPECT_EQ(allocator.getHoleCount(), 0u);
        }

        /// A block size changes where runs land, and it is what keeps one inside a single block.
        ///
        /// The same three runs with and without a block of eight: unblocked they are laid end to
        /// end, blocked the third cannot start at six and finish at eleven, so it starts the next
        /// block and the two elements it skipped stay behind as a hole.
        TEST(RtxRunAllocatorTest, aBlockSizeMovesARunThatWouldStraddleOne)
        {
            RunAllocator flat;
            flat.allocate(2);
            flat.allocate(4);
            const Rtx::Run third = flat.allocate(5);

            RunAllocator blocked(8);
            blocked.allocate(2);
            blocked.allocate(4);
            const Rtx::Run moved = blocked.allocate(5);

            EXPECT_EQ(third, (Rtx::Run{ .mOffset = 6, .mCount = 5 }));
            EXPECT_EQ(moved, (Rtx::Run{ .mOffset = 8, .mCount = 5 }));
            EXPECT_NE(third, moved) << "the block size has to change the answer or it is not doing anything";

            EXPECT_EQ(blocked.getFree(), 2u) << "six and seven, the tail of the first block";
            EXPECT_EQ(blocked.getEnd(), 13u);
        }

        /// A hole that straddles a boundary still serves a run, from the boundary onwards.
        TEST(RtxRunAllocatorTest, aRunTakesTheBlockedPartOfAHoleThatStraddlesABoundary)
        {
            RunAllocator allocator(8);

            allocator.allocate(6);
            const Rtx::Run across = allocator.allocate(8);

            // Three, because two would go straight into the tail this is about keeping.
            allocator.allocate(3);
            ASSERT_EQ(across, (Rtx::Run{ .mOffset = 8, .mCount = 8 })) << "pushed off six, which cannot hold eight";
            ASSERT_EQ(allocator.getFree(), 2u) << "six and seven";

            allocator.release(across);

            // Now one hole from six to sixteen, straddling the boundary at eight. A run of four
            // cannot start at six, so it starts at eight, and six to eight stays behind.
            ASSERT_EQ(allocator.getHoleCount(), 1u);
            EXPECT_EQ(allocator.allocate(4), (Rtx::Run{ .mOffset = 8, .mCount = 4 }));

            EXPECT_EQ(allocator.getFree(), 6u) << "six to eight, and twelve to sixteen";
            EXPECT_EQ(allocator.getHoleCount(), 2u);
        }

        /// A run exactly as long as a block sits on a boundary and fills it.
        TEST(RtxRunAllocatorTest, aRunAsLongAsABlockFillsOne)
        {
            RunAllocator allocator(8);

            EXPECT_EQ(allocator.allocate(8), (Rtx::Run{ .mOffset = 0, .mCount = 8 }));
            EXPECT_EQ(allocator.allocate(8), (Rtx::Run{ .mOffset = 8, .mCount = 8 }));
            EXPECT_EQ(allocator.getFree(), 0u) << "nothing is skipped when nothing straddles";
            EXPECT_EQ(allocator.getEnd(), 16u);
        }

        TEST(RtxRunAllocatorTest, clearingForgetsEverything)
        {
            RunAllocator allocator(8);

            const Rtx::Run first = allocator.allocate(3);
            allocator.allocate(6);
            allocator.release(first);
            ASSERT_NE(allocator.getEnd(), 0u);

            allocator.clear();

            EXPECT_EQ(allocator.getEnd(), 0u);
            EXPECT_EQ(allocator.getFree(), 0u);
            EXPECT_EQ(allocator.getHoleCount(), 0u);
            EXPECT_EQ(allocator.allocate(3), (Rtx::Run{ .mOffset = 0, .mCount = 3 }));
        }
    }
}
