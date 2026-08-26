#include <string>

#include <gtest/gtest.h>

#include <components/rtxvulkan/device.hpp>
#include <components/rtxvulkan/structurestorage.hpp>

#include "harness.hpp"

namespace Rtx
{
    namespace
    {
        /// What an acceleration structure's offset has to be a multiple of, and so the unit the
        /// storage hands out. Written here as well so the expectations below are arithmetic a reader
        /// can follow rather than a number taken from the code under test.
        constexpr VkDeviceSize sAlignment = 256;

        /// Four kilobytes: sixteen units, which is small enough to fill by hand and large enough to
        /// leave holes in.
        constexpr VkDeviceSize sBlock = 16 * sAlignment;

        /// Room is handed out in order, given back where it was, and taken up again by what fits.
        ///
        /// **Hand-computed throughout.** A structure of 1,024 bytes is four units and a structure of
        /// 2,048 is eight, so the three below fill a sixteen-unit block exactly — and the fourth has
        /// nowhere to go but a block of its own.
        TEST(RtxStructureStorageTest, roomGivenBackIsWhereTheNextStructureThatFitsGoes)
        {
            std::string reason;
            const Testing::Harness* harness = Testing::getHarness(reason);
            if (harness == nullptr)
                GTEST_SKIP() << reason;

            const Device& device = *harness->mDevice;
            StructureStorage storage(
                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                "test structures");

            const StructureRoom first = storage.take(device, 1024, sBlock);
            const StructureRoom second = storage.take(device, 2048, sBlock);
            const StructureRoom third = storage.take(device, 1024, sBlock);

            EXPECT_EQ(first.mBlock, 0u);
            EXPECT_EQ(second.mBlock, 0u);
            EXPECT_EQ(third.mBlock, 0u);
            EXPECT_EQ(storage.getOffset(first), 0u);
            EXPECT_EQ(storage.getOffset(second), 1024u);
            EXPECT_EQ(storage.getOffset(third), 3072u);
            EXPECT_EQ(storage.getBytes(), sBlock) << "three structures that fit one block asked for one block";

            // The block is full to the last unit, so this one starts another — and the buffers
            // already handed out are untouched, which is the whole reason the list grows rather than
            // the buffer.
            const StructureRoom fourth = storage.take(device, 256, sBlock);
            EXPECT_EQ(fourth.mBlock, 1u);
            EXPECT_EQ(storage.getOffset(fourth), 0u);
            EXPECT_EQ(storage.getBytes(), 2 * sBlock);
            EXPECT_NE(storage.getBuffer(first), storage.getBuffer(fourth));
            EXPECT_EQ(storage.getBuffer(first), storage.getBuffer(third)) << "one block is one buffer";

            // The eight-unit hole in the middle of the first block, taken up by the next structure
            // of exactly that size rather than appended past everything.
            storage.give(second);
            const StructureRoom again = storage.take(device, 2048, sBlock);
            EXPECT_EQ(again.mBlock, 0u);
            EXPECT_EQ(storage.getOffset(again), 1024u);
            EXPECT_EQ(storage.getBytes(), 2 * sBlock) << "reuse costs no new storage";
        }

        /// A structure larger than the block a caller asked for gets a block that holds it.
        ///
        /// **A load knows its whole total and asks for it; an arrival does not.** So the size named
        /// is a floor rather than a ceiling, and a single mesh whose structure is larger than that
        /// floor cannot be refused for it.
        TEST(RtxStructureStorageTest, aStructureLargerThanTheBlockAskedForGetsOneThatHoldsIt)
        {
            std::string reason;
            const Testing::Harness* harness = Testing::getHarness(reason);
            if (harness == nullptr)
                GTEST_SKIP() << reason;

            const Device& device = *harness->mDevice;
            StructureStorage storage(
                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                "test structures");

            const StructureRoom big = storage.take(device, 5 * sBlock, sBlock);
            EXPECT_EQ(big.mBlock, 0u);
            EXPECT_EQ(storage.getOffset(big), 0u);
            EXPECT_EQ(storage.getBytes(), 5 * sBlock);

            // And it did not become the new floor: the next block is still the size asked for.
            const StructureRoom after = storage.take(device, sBlock, sBlock);
            EXPECT_EQ(after.mBlock, 1u);
            EXPECT_EQ(storage.getBytes(), 6 * sBlock);
        }
    }
}
