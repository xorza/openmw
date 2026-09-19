#include <array>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include <boost/container/flat_set.hpp>

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

        /// **One comparator orders the rows and finds them by the key alone**, so a `flat_set` of
        /// rows keyed by a field inside each is searched with no row made up to search with. Rows
        /// on both sides, a key on either, and a key against a key: the four shapes a set asks.
        TEST(RtxKeyedLessTest, rowsAreOrderedAndFoundByTheKeyInsideThem)
        {
            boost::container::flat_set<Keyed, KeyedLess<int, KeyOfKeyed>, std::vector<Keyed>> rows;
            for (const int key : { 3, 1, 5, 4, 2 })
                rows.insert(Keyed{ .mKey = key, .mValue = key * 10 });

            ASSERT_EQ(rows.size(), 5u);
            int expected = 1;
            for (const Keyed& row : rows)
                EXPECT_EQ(row.mKey, expected++) << "the rows are not in the key's order";

            ASSERT_NE(rows.find(4), rows.end());
            EXPECT_EQ(rows.find(4)->mValue, 40);
            EXPECT_EQ(rows.find(6), rows.end());
            EXPECT_TRUE(rows.contains(2));

            // A row already keyed is refused, whatever else it carries.
            EXPECT_FALSE(rows.insert(Keyed{ .mKey = 3, .mValue = 0 }).second);
            EXPECT_EQ(rows.find(3)->mValue, 30);

            // The rows are mutable in place, which is what the ring needs of its cells.
            rows.find(5)->mValue = 55;
            EXPECT_EQ(rows.find(5)->mValue, 55);
        }

        struct Reading : Lent
        {
            int mValue = 0;

            void reuse() { reuseKeeping(*this); }
        };

        /// Fills a reading with `value`, for a take.
        auto valued(const int value)
        {
            return [value](Reading& reading) { reading.mValue = value; };
        }

        /// **A spare is held by whoever `lend`s it, and given back once the last of them releases
        /// it.** The pool counts, so every lent type has one count and one assert: a cell's, a
        /// model's and a texture's holds are the same bookkeeping.
        TEST(RtxSparesTest, anObjectIsSpareAgainWhenItsLastHolderReleasesIt)
        {
            Spares<Reading> spares;

            Reading& first = spares.take(valued(1));
            EXPECT_EQ(first.mValue, 1) << "handed out filled";
            EXPECT_EQ(first.mLent, 0u) << "taken is not yet held";
            EXPECT_EQ(spares.size(), 1u);

            spares.lend(first);
            spares.lend(first);
            EXPECT_EQ(first.mLent, 2u);

            EXPECT_FALSE(spares.release(first)) << "one holder is left";
            EXPECT_TRUE(spares.release(first)) << "the last holder";
            spares.give(first);

            // The next take is the object just given back, and nothing was made for it.
            EXPECT_EQ(&spares.take(valued(2)), &first);
            EXPECT_EQ(spares.size(), 1u);

            // A second object while the first is out is made, not shared.
            Reading& second = spares.take(valued(3));
            EXPECT_NE(&second, &first);
            EXPECT_EQ(spares.size(), 2u);
        }

        /// **A fill that throws leaves nothing taken.** The pool empties the object and has it
        /// back before the throw goes on, so the next take answers with the same object, emptied,
        /// and the pool has made nothing for it. What `CellReader::readModel` lost one model per
        /// cell to: a spare taken before a walk that threw, and never filed or given back.
        TEST(RtxSparesTest, aFillThatThrowsLeavesTheObjectSpareAndEmpty)
        {
            Spares<Reading> spares;

            Reading* attempted = nullptr;
            EXPECT_THROW(spares.take([&](Reading& reading) {
                attempted = &reading;
                reading.mValue = 7;
                throw std::runtime_error("a walk that refused the file");
            }),
                std::runtime_error);
            ASSERT_NE(attempted, nullptr);
            EXPECT_EQ(spares.size(), 1u) << "one object was made for the attempt";

            Reading& taken = spares.take(valued(0));
            EXPECT_EQ(&taken, attempted) << "the object the fill threw out of is the spare";
            EXPECT_EQ(taken.mValue, 0) << "emptied on the way back";
            EXPECT_EQ(taken.mLent, 0u);
            EXPECT_EQ(spares.size(), 1u) << "nothing was made for the second take";
        }

#ifndef NDEBUG
        /// **A sweep consumes its mark, and a take or a free spoils it.** A second sweep on one
        /// mark freed every row twice and handed one slot to two arrivals; a sweep after a take
        /// would free by a mark made of rows that were not there.
        TEST(RtxSlotRowsTest, aSweepWithoutAFreshMarkDies)
        {
            SlotRows<int> rows;
            rows.take(1);
            rows.take(2);
            rows.mark({});
            EXPECT_EQ(rows.sweep([](const Index, int&) {}), 2u);
            EXPECT_DEATH(rows.sweep([](const Index, int&) {}), "a call out of its turn");

            rows.mark({});
            rows.take(3);
            EXPECT_DEATH(rows.sweep([](const Index, int&) {}), "a call out of its turn");
        }
#endif

        /// **The pool knows which slots it holds.** A slot freed is on the list, a slot taken is
        /// not, and the two questions every table asks of it read the same byte.
        TEST(RtxSlotPoolTest, aSlotIsFreeFromItsFreeToItsTake)
        {
            SlotPool pool;
            EXPECT_FALSE(pool.isFree(3)) << "a slot nothing ever freed";
            EXPECT_EQ(pool.take(), sNoIndex);

            pool.free(3);
            pool.free(1);
            EXPECT_TRUE(pool.isFree(3));
            EXPECT_TRUE(pool.isFree(1));
            EXPECT_FALSE(pool.isFree(2));

            EXPECT_EQ(pool.take(), 1u) << "the lowest";
            EXPECT_FALSE(pool.isFree(1));
            EXPECT_TRUE(pool.isFree(3));
            EXPECT_EQ(pool.take(), 3u);
            EXPECT_FALSE(pool.isFree(3));
            EXPECT_EQ(pool.take(), sNoIndex);
        }

#ifndef NDEBUG
        /// **A slot freed twice is the pool's assert**, and not two entries on the heap handing one
        /// slot to two takers — which `VulkanRenderer::dropViewScene` could reach with a drop of a
        /// slot already dropped.
        TEST(RtxSlotPoolTest, aSlotFreedTwiceDies)
        {
            SlotPool pool;
            pool.free(2);
            EXPECT_DEATH(pool.free(2), "a slot freed twice");
        }

        /// **An object given back twice is the pool's assert**, and not two entries on the spare
        /// list handing one object to two takers. Emptied before each give, as every give-back
        /// empties: the flag is the pool's, and a reuse that reset it would hide the second give.
        TEST(RtxSparesTest, anObjectGivenBackTwiceDies)
        {
            Spares<Reading> spares;
            Reading& reading = spares.take(valued(1));
            reading.reuse();
            spares.give(reading);
            reading.reuse();
            EXPECT_DEATH(spares.give(reading), "an object given back twice");
        }
#endif
    }
}
