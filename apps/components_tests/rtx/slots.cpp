#include <array>
#include <cstddef>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtx/runs.hpp>
#include <components/rtx/scratch.hpp>
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

        struct Keyed
        {
            int mKey = 0;
            int mValue = 0;
        };

        struct KeyOfKeyed
        {
            int operator()(const Keyed& row) const { return row.mKey; }
        };

        /// **A drop hands each row over before it goes, keeps the order of the rest and shifts the
        /// table once.** The hand-over may take from the row — the ring moves a cell into its
        /// spares there — which is why this is not `std::erase_if`.
        TEST(RtxSortedRowsTest, aDropHandsEachRowOverAndKeepsTheOrderOfTheRest)
        {
            SortedRows<Keyed, int, KeyOfKeyed> rows;
            for (int key = 1; key <= 5; ++key)
                rows.insert(Keyed{ .mKey = key, .mValue = key * 10 });

            std::vector<int> taken;
            const std::size_t went = rows.dropIf([&](Keyed& row) {
                if (row.mKey % 2 != 0)
                    return false;

                taken.push_back(std::exchange(row.mValue, 0));
                return true;
            });

            EXPECT_EQ(went, 2u);
            EXPECT_EQ(taken, (std::vector<int>{ 20, 40 }));
            ASSERT_EQ(rows.size(), 3u);
            EXPECT_EQ(rows.at(1).mValue, 10);
            EXPECT_EQ(rows.at(3).mValue, 30);
            EXPECT_EQ(rows.at(5).mValue, 50);
            EXPECT_EQ(rows.find(2), nullptr);

            EXPECT_EQ(rows.dropIf([](const Keyed&) { return false; }), 0u) << "nothing to drop shifts nothing";
            EXPECT_EQ(rows.size(), 3u);
        }

        struct Reading : Lent
        {
            int mValue = 0;
        };

        /// **A spare is held by whoever `lend`s it, and given back once the last of them releases
        /// it.** The pool counts, so every lent type has one count and one assert: a cell's, a
        /// model's and a texture's holds are the same bookkeeping.
        TEST(RtxSparesTest, anObjectIsSpareAgainWhenItsLastHolderReleasesIt)
        {
            Spares<Reading> spares;

            Reading& first = spares.take();
            EXPECT_EQ(first.mLent, 0u) << "taken is not yet held";
            EXPECT_EQ(spares.size(), 1u);

            spares.lend(first);
            spares.lend(first);
            EXPECT_EQ(first.mLent, 2u);

            EXPECT_FALSE(spares.release(first)) << "one holder is left";
            EXPECT_TRUE(spares.release(first)) << "the last holder";
            spares.give(first);

            // The next take is the object just given back, and nothing was made for it.
            EXPECT_EQ(&spares.take(), &first);
            EXPECT_EQ(spares.size(), 1u);

            // A second object while the first is out is made, not shared.
            Reading& second = spares.take();
            EXPECT_NE(&second, &first);
            EXPECT_EQ(spares.size(), 2u);
        }
    }
}
