#include <gtest/gtest.h>

#include <components/rtx/spanallocator.hpp>

namespace Rtx
{
    namespace
    {
        /// Runs handed out one after another are laid end to end, and the buffer is as long as they
        /// are together.
        TEST(RtxSpanAllocatorTest, runsAreLaidEndToEndAndTheBufferIsAsLongAsTheySum)
        {
            SpanAllocator allocator;

            EXPECT_EQ(allocator.allocate(4), (Span{ .mOffset = 0, .mCount = 4 }));
            EXPECT_EQ(allocator.allocate(1), (Span{ .mOffset = 4, .mCount = 1 }));
            EXPECT_EQ(allocator.allocate(7), (Span{ .mOffset = 5, .mCount = 7 }));

            EXPECT_EQ(allocator.getEnd(), 12u);
            EXPECT_EQ(allocator.getFree(), 0u);
            EXPECT_EQ(allocator.getHoleCount(), 0u);
        }

        /// A hole is filled by the run that fits it worst-but-still, not by the first one offered.
        ///
        /// Two holes of four and ten, and a run of four: first fit would take the ten and leave a
        /// four-long hole that the next four-long run has to append past, so the buffer grows.
        /// Best fit takes the four, and the ten is still there for something ten long.
        TEST(RtxSpanAllocatorTest, aRunGoesInTheSmallestHoleThatHoldsIt)
        {
            SpanAllocator allocator;

            const Span first = allocator.allocate(10);
            allocator.allocate(1);
            const Span small = allocator.allocate(4);
            allocator.allocate(1);
            ASSERT_EQ(allocator.getEnd(), 16u);

            allocator.release(first);
            allocator.release(small);
            ASSERT_EQ(allocator.getHoleCount(), 2u);

            // The four-long hole, which is at eleven: ten, one, then four.
            EXPECT_EQ(allocator.allocate(4), (Span{ .mOffset = 11, .mCount = 4 }));

            // And the ten is intact, so nothing had to be appended.
            EXPECT_EQ(allocator.allocate(10), (Span{ .mOffset = 0, .mCount = 10 }));
            EXPECT_EQ(allocator.getEnd(), 16u);
        }

        /// A run that does not fill its hole leaves the rest of it behind.
        TEST(RtxSpanAllocatorTest, aRunSmallerThanItsHoleLeavesTheRemainder)
        {
            SpanAllocator allocator;

            const Span wide = allocator.allocate(10);
            allocator.allocate(1);
            allocator.release(wide);

            EXPECT_EQ(allocator.allocate(3), (Span{ .mOffset = 0, .mCount = 3 }));
            EXPECT_EQ(allocator.getFree(), 7u);
            EXPECT_EQ(allocator.getHoleCount(), 1u);

            EXPECT_EQ(allocator.allocate(7), (Span{ .mOffset = 3, .mCount = 7 }));
            EXPECT_EQ(allocator.getFree(), 0u);
            EXPECT_EQ(allocator.getEnd(), 11u);
        }

        /// Runs given back beside one another become one hole, whichever order they come back in.
        ///
        /// **This is what a cell leaving is.** It arrived as thousands of small runs laid end to
        /// end and it leaves as thousands of releases; without merging, the next cell would find
        /// thousands of holes none of which is big enough for anything, and would append past all
        /// of them.
        TEST(RtxSpanAllocatorTest, releasesThatTouchBecomeOneHole)
        {
            for (const bool backwards : { false, true })
            {
                SpanAllocator allocator;

                const Span a = allocator.allocate(3);
                const Span b = allocator.allocate(3);
                const Span c = allocator.allocate(3);
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
                EXPECT_EQ(allocator.allocate(9), (Span{ .mOffset = 0, .mCount = 9 })) << "backwards: " << backwards;
            }
        }

        /// A hole that reaches the end of the buffer shortens the buffer instead of staying a hole.
        TEST(RtxSpanAllocatorTest, aHoleAtTheEndGivesTheRoomBack)
        {
            SpanAllocator allocator;

            allocator.allocate(5);
            const Span last = allocator.allocate(6);
            ASSERT_EQ(allocator.getEnd(), 11u);

            allocator.release(last);

            EXPECT_EQ(allocator.getEnd(), 5u) << "the buffer is as long as what is in it";
            EXPECT_EQ(allocator.getHoleCount(), 0u);
            EXPECT_EQ(allocator.getFree(), 0u);
        }

        /// The run before the end goes with it: merging first, shrinking second.
        TEST(RtxSpanAllocatorTest, aHoleMergedIntoTheEndGivesBothBack)
        {
            SpanAllocator allocator;

            allocator.allocate(2);
            const Span middle = allocator.allocate(3);
            const Span last = allocator.allocate(4);

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
        TEST(RtxSpanAllocatorTest, aBlockSizeMovesARunThatWouldStraddleOne)
        {
            SpanAllocator flat;
            flat.allocate(2);
            flat.allocate(4);
            const Span third = flat.allocate(5);

            SpanAllocator blocked(8);
            blocked.allocate(2);
            blocked.allocate(4);
            const Span moved = blocked.allocate(5);

            EXPECT_EQ(third, (Span{ .mOffset = 6, .mCount = 5 }));
            EXPECT_EQ(moved, (Span{ .mOffset = 8, .mCount = 5 }));
            EXPECT_NE(third, moved) << "the block size has to change the answer or it is not doing anything";

            EXPECT_EQ(blocked.getFree(), 2u) << "six and seven, the tail of the first block";
            EXPECT_EQ(blocked.getEnd(), 13u);
        }

        /// A hole that straddles a boundary still serves a run, from the boundary onwards.
        TEST(RtxSpanAllocatorTest, aRunTakesTheBlockedPartOfAHoleThatStraddlesABoundary)
        {
            SpanAllocator allocator(8);

            allocator.allocate(6);
            const Span across = allocator.allocate(8);

            // Three, because two would go straight into the tail this is about keeping.
            allocator.allocate(3);
            ASSERT_EQ(across, (Span{ .mOffset = 8, .mCount = 8 })) << "pushed off six, which cannot hold eight";
            ASSERT_EQ(allocator.getFree(), 2u) << "six and seven";

            allocator.release(across);

            // Now one hole from six to sixteen, straddling the boundary at eight. A run of four
            // cannot start at six, so it starts at eight, and six to eight stays behind.
            ASSERT_EQ(allocator.getHoleCount(), 1u);
            EXPECT_EQ(allocator.allocate(4), (Span{ .mOffset = 8, .mCount = 4 }));

            EXPECT_EQ(allocator.getFree(), 6u) << "six to eight, and twelve to sixteen";
            EXPECT_EQ(allocator.getHoleCount(), 2u);
        }

        /// A run exactly as long as a block sits on a boundary and fills it.
        TEST(RtxSpanAllocatorTest, aRunAsLongAsABlockFillsOne)
        {
            SpanAllocator allocator(8);

            EXPECT_EQ(allocator.allocate(8), (Span{ .mOffset = 0, .mCount = 8 }));
            EXPECT_EQ(allocator.allocate(8), (Span{ .mOffset = 8, .mCount = 8 }));
            EXPECT_EQ(allocator.getFree(), 0u) << "nothing is skipped when nothing straddles";
            EXPECT_EQ(allocator.getEnd(), 16u);
        }

        TEST(RtxSpanAllocatorTest, clearingForgetsEverything)
        {
            SpanAllocator allocator(8);

            const Span first = allocator.allocate(3);
            allocator.allocate(6);
            allocator.release(first);
            ASSERT_NE(allocator.getEnd(), 0u);

            allocator.clear();

            EXPECT_EQ(allocator.getEnd(), 0u);
            EXPECT_EQ(allocator.getFree(), 0u);
            EXPECT_EQ(allocator.getHoleCount(), 0u);
            EXPECT_EQ(allocator.allocate(3), (Span{ .mOffset = 0, .mCount = 3 }));
        }
    }
}
