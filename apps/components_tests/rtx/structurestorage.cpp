#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include <vulkan/vulkan_core.h>

#include <components/rtx/result.hpp>
#include <components/rtxvulkan/device.hpp>
#include <components/rtxvulkan/memory.hpp>
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

        struct RtxStructureStorageTest : Testing::DeviceTest
        {
        };

        /// Room is handed out in order, given back where it was, and taken up again by what fits.
        ///
        /// **Hand-computed throughout.** A structure of 1,024 bytes is four units and a structure of
        /// 2,048 is eight, so the three below fill a sixteen-unit block exactly — and the fourth has
        /// nowhere to go but a block of its own.
        TEST_F(RtxStructureStorageTest, roomGivenBackIsWhereTheNextStructureThatFitsGoes)
        {
            const Device& device = getDevice();
            StructureStorage storage(
                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                "test structures");

            const StructureRoom first = storage.take(device, 1024, sBlock).value();
            const StructureRoom second = storage.take(device, 2048, sBlock).value();
            const StructureRoom third = storage.take(device, 1024, sBlock).value();

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
            const StructureRoom fourth = storage.take(device, 256, sBlock).value();
            EXPECT_EQ(fourth.mBlock, 1u);
            EXPECT_EQ(storage.getOffset(fourth), 0u);
            EXPECT_EQ(storage.getBytes(), 2 * sBlock);
            EXPECT_NE(storage.getBuffer(first), storage.getBuffer(fourth));
            EXPECT_EQ(storage.getBuffer(first), storage.getBuffer(third)) << "one block is one buffer";

            // The eight-unit hole in the middle of the first block, taken up by the next structure
            // of exactly that size rather than appended past everything.
            storage.give(second);
            const StructureRoom again = storage.take(device, 2048, sBlock).value();
            EXPECT_EQ(again.mBlock, 0u);
            EXPECT_EQ(storage.getOffset(again), 1024u);
            EXPECT_EQ(storage.getBytes(), 2 * sBlock) << "reuse costs no new storage";
        }

        /// A structure larger than the block a caller asked for gets a block that holds it.
        ///
        /// **A load knows its whole total and asks for it; an arrival does not.** So the size named
        /// is a floor rather than a ceiling, and a single mesh whose structure is larger than that
        /// floor cannot be refused for it.
        TEST_F(RtxStructureStorageTest, aStructureLargerThanTheBlockAskedForGetsOneThatHoldsIt)
        {
            const Device& device = getDevice();
            StructureStorage storage(
                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                "test structures");

            const StructureRoom big = storage.take(device, 5 * sBlock, sBlock).value();
            EXPECT_EQ(big.mBlock, 0u);
            EXPECT_EQ(storage.getOffset(big), 0u);
            EXPECT_EQ(storage.getBytes(), 5 * sBlock);

            // And it did not become the new floor: the next block is still the size asked for.
            const StructureRoom after = storage.take(device, sBlock, sBlock).value();
            EXPECT_EQ(after.mBlock, 1u);
            EXPECT_EQ(storage.getBytes(), 6 * sBlock);
        }

        /// A structure the device has no room for is refused, and a block it has no room for is
        /// asked for again at the structure's own size.
        ///
        /// **No gap in any block of content's, and then room for one more block.** With the ceiling
        /// at nought the structure has nowhere at all, and no block joins the list. A load asks for
        /// its whole total, here two hundred megabytes — an allocation of its own, past the room —
        /// and is given a block of the structure's four units instead, out of the one block
        /// content may open.
        TEST_F(RtxStructureStorageTest, aBlockTheDeviceHasNoRoomForIsAskedForAgainAtTheStructuresSize)
        {
            const Device& device = getDevice();
            MemoryAllocator& memory = device.getMemory();
            StructureStorage storage(
                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                "test structures");

            const Testing::NoRoomForContent full(device);

            const Result<StructureRoom, std::string_view> none = storage.take(device, 1024, 1024);
            ASSERT_FALSE(none.isOk());
            EXPECT_EQ(none.error(), "no device memory is left for it");
            EXPECT_EQ(storage.getBytes(), 0u) << "a refused block joined the list";

            const Testing::BudgetLimit oneBlock(
                memory, Testing::budgetAbove(memory, MemoryUse::Structure, VkDeviceSize{ 65 } << 20));

            const Result<StructureRoom, std::string_view> room = storage.take(device, 1024, VkDeviceSize{ 200 } << 20);
            ASSERT_TRUE(room.isOk());
            EXPECT_EQ(storage.getOffset(room.value()), 0u);
            EXPECT_EQ(storage.getBytes(), 1024u) << "the block was not made at the structure's size";
        }
    }
}
