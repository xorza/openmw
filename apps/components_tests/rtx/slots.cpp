#include <array>
#include <cstddef>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtx/slots.hpp>

namespace Rtx
{
    namespace
    {
        /// **A held row survives a sweep it was not named to, and goes on the first after the last
        /// hold is given back.** What every table counts its holders by, so it is proved once here:
        /// a texture named by materials, a rig stood on by meshes, a ground row held by the ring.
        TEST(RtxSlotRowsTest, aHeldRowIsASurvivorTheMarkDidNotName)
        {
            SlotRows<int> rows;
            const Index first = rows.take(1);
            const Index second = rows.take(2);
            const Index third = rows.take(3);
            ASSERT_EQ(rows.getLiveCount(), 3u);

            rows.hold(second);
            rows.hold(second);
            EXPECT_EQ(rows.getHolds(second), 2u);
            EXPECT_FALSE(rows.hasDroppedHolds());

            // Named: the first. Held: the second. The third is nobody's.
            const std::array<Index, 1> named{ first };
            EXPECT_EQ(rows.mark(named), 2u) << "one named and one held are two distinct survivors";

            std::vector<Index> freed;
            EXPECT_EQ(rows.sweep([&](const Index index, int&) { freed.push_back(index); }), 1u);
            ASSERT_EQ(freed.size(), 1u);
            EXPECT_EQ(freed[0], third);
            EXPECT_EQ(rows.getLiveCount(), 2u);

            // One hold back is still held; the last is the drop the next sweep is owed for.
            EXPECT_FALSE(rows.drop(second));
            EXPECT_FALSE(rows.hasDroppedHolds());
            EXPECT_TRUE(rows.drop(second));
            EXPECT_TRUE(rows.hasDroppedHolds());

            EXPECT_EQ(rows.mark(named), 1u) << "nothing holds the second now";
            EXPECT_FALSE(rows.hasDroppedHolds()) << "the mark is what the drop was owed to";

            freed.clear();
            EXPECT_EQ(rows.sweep([&](const Index index, int&) { freed.push_back(index); }), 1u);
            ASSERT_EQ(freed.size(), 1u);
            EXPECT_EQ(freed[0], second);

            // A freed slot is taken over with no holds, whatever its last tenant carried.
            const Index again = rows.take(4);
            EXPECT_EQ(again, second) << "the lowest free slot";
            EXPECT_EQ(rows.getHolds(again), 0u);
        }
    }
}
