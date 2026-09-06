#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtxvulkan/buffer.hpp>
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

            batch.flush();
        }

        /// A batch that leaves during unwinding submits nothing.
        ///
        /// **What a constructor that fails half way leaves behind.** `Texture` records the upload of
        /// its primary image and then makes a second one; where that allocation throws, the image is
        /// destroyed by the unwinding and a destructor that submitted would carry a copy naming a
        /// handle that has gone. The recording goes back to the pool instead.
        TEST_F(RtxBatchTest, aBatchAbandonedByAnExceptionSubmitsNothing)
        {
            const Buffer source = Buffer::staging(getDevice(), sizeof(std::uint32_t),
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            *static_cast<std::uint32_t*>(source.map()) = 0x5eaf00d;

            const Buffer target = Buffer::staging(getDevice(), sizeof(std::uint32_t),
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            *static_cast<std::uint32_t*>(target.map()) = 0;

            struct Abandoned
            {
            };

            EXPECT_THROW(
                {
                    Batch batch(getPool());

                    const VkBufferCopy whole{ .size = sizeof(std::uint32_t) };
                    vkCmdCopyBuffer(batch.getCommands(), source.getHandle(), target.getHandle(), 1, &whole);

                    throw Abandoned{};
                },
                Abandoned);

            // The pool is asked for a submit of its own, so anything the batch had left behind would
            // have run by the time this returns.
            getPool().submitAndWait([](VkCommandBuffer) {});

            EXPECT_EQ(*static_cast<const std::uint32_t*>(target.map()), 0u)
                << "an abandoned batch's copy reached the device";
        }
    }
}
