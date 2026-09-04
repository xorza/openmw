#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtxvulkan/buffer.hpp>
#include <components/rtxvulkan/commands.hpp>
#include <components/rtxvulkan/device.hpp>
#include <components/rtxvulkan/graveyard.hpp>
#include <components/rtxvulkan/slottable.hpp>

#include "harness.hpp"

namespace Rtx
{
    namespace
    {
        struct TestRow
        {
            std::uint32_t mValue = 0;
        };

        /// What a copy would be written from, and what it would be written with.
        ///
        /// **These tests ask the bookkeeping and not the picture.** `Buffer` is write-combining
        /// and hands out no readable pointer, deliberately, so what a copy actually holds cannot be
        /// read back at any sensible cost. What can be checked is the debt — which rows a copy is
        /// about to be given — and that is where every one of the failures this type replaced lived:
        /// a copy that was never told about a row it had to have.
        class RtxSlotTableTest : public Testing::DeviceTest
        {
        protected:
            void SetUp() override
            {
                Testing::DeviceTest::SetUp();
                if (mHarness == nullptr)
                    return;

                mGraveyard = std::make_unique<Graveyard>(getDevice(), getPool());
                mTable.open(getDevice(), 2, VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, "test");
            }

            /// Which rows `slot` is owed, sorted and with the duplicates a repeated write leaves.
            std::vector<Index> owedBy(std::uint32_t slot)
            {
                const std::span<const Index> owed = mTable.getOwed(slot);
                std::vector<Index> sorted(owed.begin(), owed.end());
                std::sort(sorted.begin(), sorted.end());
                sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
                return sorted;
            }

            void sync(std::uint32_t slot) { mTable.sync(slot, *mGraveyard); }

            std::unique_ptr<Graveyard> mGraveyard;
            SlotTable<TestRow> mTable;
        };

        /// A row written is owed by every copy, and paying one copy leaves the other owing it.
        ///
        /// This is the failure that put terrain a frame behind: one table subscribed to a narrower
        /// set of the scene's change lists than the other, so its second copy was never told.
        TEST_F(RtxSlotTableTest, aRowWrittenIsOwedByEveryCopyUntilThatCopyIsSynced)
        {
            mTable.resize(4);
            sync(0);
            sync(1);
            ASSERT_FALSE(mTable.owes(0));
            ASSERT_FALSE(mTable.owes(1));

            mTable.write(2).mValue = 7;

            EXPECT_EQ(owedBy(0), (std::vector<Index>{ 2 }));
            EXPECT_EQ(owedBy(1), (std::vector<Index>{ 2 }));

            sync(0);
            EXPECT_FALSE(mTable.owes(0)) << "the copy that was paid still owes";
            EXPECT_EQ(owedBy(1), (std::vector<Index>{ 2 })) << "the copy that was not paid forgot";

            sync(1);
            EXPECT_FALSE(mTable.owes(1));
        }

        /// A copy owes every row written since it was last paid, however many frames that spans.
        ///
        /// The second copy is written every other frame, so what it owes is two frames of changes
        /// and not one. A debt taken from the current frame's list alone loses the older half.
        TEST_F(RtxSlotTableTest, aCopyOwesEveryRowWrittenSinceItWasLastPaid)
        {
            mTable.resize(8);
            sync(0);
            sync(1);

            mTable.write(1);
            sync(0);
            mTable.write(3);
            sync(0);
            mTable.write(5);

            EXPECT_EQ(owedBy(0), (std::vector<Index>{ 5 })) << "the copy paid twice owes only the last";
            EXPECT_EQ(owedBy(1), (std::vector<Index>{ 1, 3, 5 })) << "three frames of changes, one copy";

            sync(1);
            EXPECT_FALSE(mTable.owes(1));
        }

        /// Growing owes the appended rows and nothing else: a row keeps its offset.
        TEST_F(RtxSlotTableTest, growingOwesWhatWasAppendedAndNotWhatWasAlreadyThere)
        {
            mTable.resize(3);
            sync(0);
            sync(1);

            mTable.resize(6);

            EXPECT_EQ(owedBy(0), (std::vector<Index>{ 3, 4, 5 }));
            EXPECT_EQ(owedBy(1), (std::vector<Index>{ 3, 4, 5 }));
            EXPECT_FALSE(mTable.owesEverything(0)) << "a growth rewrote rows that had not moved";
        }

        /// Shrinking owes nothing. The rows past the end are not read, so nothing has to be said
        /// about them — and the table is never compacted, so nothing below the end has moved.
        TEST_F(RtxSlotTableTest, shrinkingOwesNothing)
        {
            mTable.resize(6);
            sync(0);
            sync(1);

            mTable.resize(2);

            EXPECT_FALSE(mTable.owes(0));
            EXPECT_FALSE(mTable.owes(1));
            EXPECT_EQ(mTable.size(), 2u);
        }

        /// A copy that has never been written owes the whole table, and paying it clears that.
        TEST_F(RtxSlotTableTest, aCopyNothingHasWrittenOwesTheWholeTable)
        {
            EXPECT_TRUE(mTable.owesEverything(0));
            EXPECT_TRUE(mTable.owesEverything(1));

            mTable.resize(5);
            mTable.write(0).mValue = 1;

            EXPECT_TRUE(mTable.owesEverything(0)) << "a row named where the whole table is owed";

            sync(0);
            EXPECT_FALSE(mTable.owes(0));
            EXPECT_TRUE(mTable.owesEverything(1)) << "paying one copy answered for the other";
        }

        /// The rows are the one answer every copy is written from, so a write is visible in them at
        /// once and a copy paid later reads the value as it then stands rather than as it was.
        TEST_F(RtxSlotTableTest, theRowsAreTheOneAnswerEveryCopyIsWrittenFrom)
        {
            mTable.resize(2);
            sync(0);
            sync(1);

            mTable.write(1).mValue = 10;
            sync(0);
            mTable.write(1).mValue = 20;

            EXPECT_EQ(mTable.getRows()[1].mValue, 20u);
            EXPECT_EQ(owedBy(1), (std::vector<Index>{ 1 })) << "one row named twice is one row to write";

            sync(1);
            EXPECT_EQ(mTable.getRows()[1].mValue, 20u) << "syncing changed the answer";
        }

        /// A copy's buffer grows with the table and never with the frame count.
        ///
        /// **Doubling has to be asked for and not taken.** A growth strategy applied whether or not
        /// the buffer is too small remakes it on every sync, twice as large each time, and a run of
        /// a few hundred frames ends at `VK_ERROR_OUT_OF_DEVICE_MEMORY`.
        TEST_F(RtxSlotTableTest, syncingWithoutGrowingLeavesTheBufferWhereItIs)
        {
            mTable.resize(64);
            sync(0);

            const VkDeviceSize settled = mTable.getCopyBytes(0);
            ASSERT_GE(settled, 64 * sizeof(TestRow));

            for (int frame = 0; frame < 8; ++frame)
            {
                mTable.write(1).mValue = static_cast<std::uint32_t>(frame);
                sync(0);
            }

            EXPECT_EQ(mTable.getCopyBytes(0), settled) << "the buffer was made again by a sync that fitted";
        }

        /// A table that keeps growing is made again a logarithmic number of times, not once a row.
        TEST_F(RtxSlotTableTest, aTableThatKeepsGrowingDoublesRatherThanFollowingEachRow)
        {
            mTable.resize(1);
            sync(0);

            VkDeviceSize remade = 0;
            VkDeviceSize was = mTable.getCopyBytes(0);
            for (std::size_t rows = 2; rows <= 512; ++rows)
            {
                mTable.resize(rows);
                sync(0);
                if (mTable.getCopyBytes(0) != was)
                {
                    ++remade;
                    was = mTable.getCopyBytes(0);
                }
            }

            // 512 rows reached by doubling from one is nine growths, and the count must not depend
            // on how many rows were added between them.
            EXPECT_LE(remade, 10u) << "the buffer followed the row count instead of doubling";
            EXPECT_GE(mTable.getCopyBytes(0), 512 * sizeof(TestRow));
        }

        /// Blocks keep the same account as rows: named by `write`, cleared only by `sync`.
        TEST_F(RtxSlotTableTest, blocksOweEveryRunNamedSinceThatCopyWasLastFilled)
        {
            SlotBlocks blocks(64, sizeof(std::uint32_t));
            blocks.open(*mHarness->mDevice, 2, VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, "test blocks");
            blocks.reserve(128);

            blocks.write(2);
            blocks.write(5);

            std::vector<Index> filled;
            blocks.sync(0, [&](const Index at, BlockedBuffer&) { filled.push_back(at); });
            EXPECT_EQ(filled, (std::vector<Index>{ 2, 5 }));
            EXPECT_TRUE(blocks.getOwed(0).empty()) << "the copy that was filled still owes";

            blocks.write(9);

            filled.clear();
            blocks.sync(1, [&](const Index at, BlockedBuffer&) { filled.push_back(at); });
            EXPECT_EQ(filled, (std::vector<Index>{ 2, 5, 9 })) << "the copy that was not filled forgot two runs";
        }

        /// `settle` says a copy holds everything there is, which is how a load ends.
        TEST_F(RtxSlotTableTest, settlingSaysACopyHoldsEverythingThereIs)
        {
            SlotBlocks blocks(64, sizeof(std::uint32_t));
            blocks.open(*mHarness->mDevice, 2, VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, "test blocks");
            blocks.reserve(128);

            blocks.write(3);
            blocks.settle(0);

            std::vector<Index> filled;
            blocks.sync(0, [&](const Index at, BlockedBuffer&) { filled.push_back(at); });
            EXPECT_TRUE(filled.empty()) << "a settled copy was filled again";

            blocks.sync(1, [&](const Index at, BlockedBuffer&) { filled.push_back(at); });
            EXPECT_EQ(filled, (std::vector<Index>{ 3 })) << "settling one copy answered for the other";
        }

        /// A table with nothing in it still has a buffer, because the frame carries an address for it.
        TEST_F(RtxSlotTableTest, aTableWithNoRowsStillHasABufferToAddress)
        {
            sync(0);
            EXPECT_NE(mTable.getDeviceAddress(0), 0u);
        }
    }
}
