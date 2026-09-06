#include <cstddef>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtxvulkan/commands.hpp>
#include <components/rtxvulkan/device.hpp>

#include "harness.hpp"

namespace Rtx
{
    namespace
    {
        struct RtxBatchTest : Testing::DeviceTest
        {
        };

        /// One block takes upload after upload, and a new one is taken only where the last cannot
        /// hold what is asked of it.
        ///
        /// **What a buffer apiece cost.** A cell that arrives with two hundred textures uploads four
        /// hundred times — the levels and the shading map of each — and each of those was a
        /// `vkCreateBuffer`, a `vkAllocateMemory` and a `vkMapMemory` on the frame the cell landed.
        /// The bytes are alive just as long either way: the batch has held every one of them until
        /// the submit since it was written.
        ///
        /// **Appended and never rewound**, because nothing has run when a batch is being recorded:
        /// a block reused inside one would have the copy of an earlier upload reading what a later
        /// one wrote over it.
        TEST_F(RtxBatchTest, oneBlockTakesUploadAfterUploadAndAnotherOnlyWhereOneWillNotFit)
        {
            Batch batch(getPool());

            const std::vector<std::byte> hundred(100, std::byte{ 1 });
            const std::vector<std::byte> fifty(50, std::byte{ 2 });

            const StagingRun first = batch.stage(getDevice(), hundred);
            const StagingRun second = batch.stage(getDevice(), fifty);

            EXPECT_EQ(first.mOffset, 0u);
            EXPECT_EQ(second.mBuffer, first.mBuffer) << "a second upload took a buffer of its own";

            // A hundred rounded up to the sixteen a copy offset has to start on.
            EXPECT_EQ(second.mOffset, 112u);

            // **Past the block rather than rounded up to it**, so an upload larger than one gets a
            // block of its own exactly its size — which leaves the upload after it nowhere to go
            // but a block of its own as well.
            const std::vector<std::byte> past(sStagingBlock + 1, std::byte{ 3 });
            const StagingRun alone = batch.stage(getDevice(), past);
            const StagingRun after = batch.stage(getDevice(), fifty);

            EXPECT_NE(alone.mBuffer, first.mBuffer) << "an upload landed in a block with no room for it";
            EXPECT_EQ(alone.mOffset, 0u);
            EXPECT_NE(after.mBuffer, alone.mBuffer) << "an upload landed in a block with no room for it";
            EXPECT_EQ(after.mOffset, 0u);
        }
    }
}
