#include <cstdint>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtx/mirroridentity.hpp>
#include <components/rtx/walk.hpp>

namespace Rtx
{
    namespace
    {
        using Map = std::unordered_map<int, Known>;

        struct RtxKeptTest : testing::Test
        {
            MirrorPass mPass;
            Kept<Map> mKept{ mPass };

            /// What the extractor does between two walks: the sweep, then the epoch.
            void nextEpoch() { ++mPass.mEpoch; }

            std::vector<Index> mLive;
        };

        /// **A held entry survives a sweep whatever its stamp, and counts as reached for `whole`.**
        /// The cell ring stands models the walk never meets, so without this the sweep after every
        /// frame would take them and the count would call for a sweep on every frame besides.
        TEST_F(RtxKeptTest, aHeldEntrySurvivesTheSweepAndIsWholeUnstamped)
        {
            mKept.add(1, Known{ .mIndex = 10 });
            mKept.add(2, Known{ .mIndex = 20 });
            EXPECT_TRUE(mKept.whole()) << "two entries, both stamped by the epoch that added them";

            mKept.hold(mKept.find(2));
            EXPECT_TRUE(mKept.whole()) << "a hold on a stamped entry changes nothing this epoch";

            nextEpoch();
            EXPECT_FALSE(mKept.whole()) << "the unheld entry is unreached in the new epoch";

            mKept.stamp(mKept.find(1));
            EXPECT_TRUE(mKept.whole()) << "one reached and one held is the whole map";

            // A stamp on a held entry is not counted twice.
            mKept.stamp(mKept.find(2));
            EXPECT_TRUE(mKept.whole());

            nextEpoch();
            mKept.stamp(mKept.find(1));
            EXPECT_EQ(mKept.sweep(mLive), 0u) << "nothing to drop: one stamped, one held";
            EXPECT_EQ(mLive.size(), 2u) << "the held entry is a survivor the release keeps";
        }

        /// **A dropped hold is a sweep owed**, even on an epoch that reached everything else: the
        /// entry is stale the moment nothing holds it, and `whole` has to say so or the row it names
        /// leaks until something else dies.
        TEST_F(RtxKeptTest, droppingTheLastHoldOwesASweepThatTakesTheEntry)
        {
            mKept.add(1, Known{ .mIndex = 10 });
            mKept.hold(mKept.find(1));
            mKept.hold(mKept.find(1));

            nextEpoch();
            EXPECT_TRUE(mKept.whole()) << "a held map with nothing unheld in it is whole before any stamp";

            mKept.drop(mKept.find(1));
            EXPECT_TRUE(mKept.whole()) << "one hold of two given back keeps the entry held";

            mKept.drop(mKept.find(1));
            EXPECT_FALSE(mKept.whole()) << "the last hold went and the entry carries an old stamp";

            std::uint32_t dropped = 0;
            mKept.retire([&](const Known& gone) {
                EXPECT_EQ(gone.mIndex, 10u);
                ++dropped;
            });
            EXPECT_EQ(dropped, 1u);
            EXPECT_TRUE(mKept.whole()) << "an empty map is whole";

            // The other way round: a hold dropped on an entry the epoch stamped is a reached entry
            // again, and owes nothing.
            mKept.add(2, Known{ .mIndex = 20 });
            mKept.hold(mKept.find(2));
            mKept.drop(mKept.find(2));
            EXPECT_TRUE(mKept.whole());
            EXPECT_EQ(mKept.sweep(mLive), 0u);
            EXPECT_EQ(mLive.size(), 1u);
        }

        /// A held entry abandoned mid-walk comes off the held count, or every sweep after would
        /// find the map one short of whole for ever.
        TEST_F(RtxKeptTest, abandoningAHeldEntryCountsItOff)
        {
            mKept.add(1, Known{ .mIndex = 10 });
            mKept.hold(mKept.find(1));
            mKept.add(2, Known{ .mIndex = 20 });

            mKept.abandon(mKept.find(1));
            EXPECT_FALSE(mKept.whole()) << "an abandon owes a sweep";

            EXPECT_EQ(mKept.sweep(mLive), 0u) << "the abandoned entry is already gone";
            EXPECT_EQ(mLive.size(), 1u);
            EXPECT_TRUE(mKept.whole());

            nextEpoch();
            mKept.stamp(mKept.find(2));
            EXPECT_TRUE(mKept.whole()) << "no held entry is counted that is not there";
        }
    }
}
