#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtxvulkan/device.hpp>
#include <components/rtxvulkan/memory.hpp>

#include "harness.hpp"

namespace Rtx
{
    namespace
    {
        struct RtxMemoryTest : Testing::DeviceTest
        {
        };

        /// What a resource of `size` needing `alignment` asks for, with every memory type allowed.
        ///
        /// The allocator is asked directly rather than through a buffer or an image, because what is
        /// under test is where a range lands and a resource would only report where it was bound.
        VkMemoryRequirements asks(VkDeviceSize size, VkDeviceSize alignment)
        {
            return VkMemoryRequirements{ .size = size, .alignment = alignment, .memoryTypeBits = ~0u };
        }

        /// A shading map's own requirements, measured on this hardware: two kilobytes at a
        /// kilobyte's alignment. The smallest image a cell brings, and the one a cell brings most of.
        VkMemoryRequirements asksLikeAShadingMap()
        {
            return asks(2048, 1024);
        }

        /// A thousand shading maps come out of one allocation rather than a thousand.
        ///
        /// **What this allocator exists for.** A cell of Morrowind brings a few hundred textures and
        /// each carries a map, so an allocation apiece is four figures of kernel-visible calls for
        /// the cell the game starts in, each for two kilobytes the driver rounds up to its own
        /// granularity. Two megabytes of maps fit inside the smallest block a pool starts with, so
        /// the honest claim is one and the assertion allows no more.
        TEST_F(RtxMemoryTest, aThousandSmallImagesComeOutOfOneAllocation)
        {
            MemoryAllocator& memory = getDevice().getMemory();
            const std::size_t before = memory.getBlockCount();

            std::vector<DeviceMemory> held;
            held.reserve(1000);
            for (int at = 0; at < 1000; ++at)
                held.push_back(
                    memory.take(asksLikeAShadingMap(), VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, Tiling::Optimal));

            // At most one, and not exactly one: a test that ran before this may have left a block of
            // the same pool with room in it, and taking that room is the allocator working.
            EXPECT_LE(memory.getBlockCount() - before, 1u) << "a thousand small images made more than one allocation";

            // And they are a thousand distinct places: same block, ascending offsets, no two
            // overlapping. 2048 bytes at a 1024-byte alignment is two whole pages, so consecutive
            // ranges sit exactly 2048 apart.
            for (std::size_t at = 1; at < held.size(); ++at)
            {
                if (held[at].getHandle() != held[at - 1].getHandle())
                    continue;

                EXPECT_GE(held[at].getOffset(), held[at - 1].getOffset() + 2048)
                    << "range " << at << " overlaps the one before it";
            }
        }

        /// A range given back is what the next resource of that size takes.
        ///
        /// **What makes a cell leaving pay for the cell arriving.** The free list merges what it is
        /// handed, so the hole a departing texture leaves is the hole the next one lands in — no
        /// call into the driver, and no block that grows for ever.
        TEST_F(RtxMemoryTest, aRangeGivenBackIsTakenOverByTheNextResource)
        {
            MemoryAllocator& memory = getDevice().getMemory();

            VkDeviceMemory handle = VK_NULL_HANDLE;
            VkDeviceSize offset = 0;
            {
                const DeviceMemory first
                    = memory.take(asksLikeAShadingMap(), VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, Tiling::Optimal);
                handle = first.getHandle();
                offset = first.getOffset();
            }

            const std::size_t after = memory.getBlockCount();
            const DeviceMemory second
                = memory.take(asksLikeAShadingMap(), VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, Tiling::Optimal);

            EXPECT_EQ(second.getHandle(), handle) << "the range came from a different allocation";
            EXPECT_EQ(second.getOffset(), offset) << "the range that was given back was not taken over";
            EXPECT_EQ(memory.getBlockCount(), after) << "taking over a hole made a new allocation";
        }

        /// A buffer and an image never share an allocation.
        ///
        /// **`bufferImageGranularity` settled once instead of at every placement.** Where the two
        /// share an allocation Vulkan requires a whole granularity between them, and a pool apiece is
        /// what makes the question unaskable rather than a check that could be got wrong.
        TEST_F(RtxMemoryTest, aBufferAndAnImageNeverShareAnAllocation)
        {
            MemoryAllocator& memory = getDevice().getMemory();

            const DeviceMemory linear
                = memory.take(asks(4096, 256), VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, Tiling::Linear);
            const DeviceMemory tiled
                = memory.take(asks(4096, 1024), VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, Tiling::Optimal);

            EXPECT_NE(linear.getHandle(), tiled.getHandle()) << "the two tilings landed in one allocation";
        }

        /// An alignment coarser than a page is honoured, and costs only what it has to.
        ///
        /// **The case this hardware never asks for and the code still owes.** Every image here wants
        /// 1024, which is the page, so nothing measured exercises this — a driver that wanted 64 KiB
        /// would place a range at an offset that is not a multiple of it unless the extra pages are
        /// taken.
        TEST_F(RtxMemoryTest, anAlignmentCoarserThanAPageIsHonoured)
        {
            MemoryAllocator& memory = getDevice().getMemory();
            constexpr VkDeviceSize coarse = 64 * 1024;

            std::vector<DeviceMemory> held;
            for (int at = 0; at < 8; ++at)
                held.push_back(memory.take(asks(4096, coarse), VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, Tiling::Optimal));

            for (std::size_t at = 0; at < held.size(); ++at)
            {
                EXPECT_EQ(held[at].getOffset() % coarse, 0u) << "range " << at << " was not aligned as it asked";

                // Four kilobytes at a 64 KiB alignment is four pages plus the sixty-three an
                // alignment that coarse may skip, so a range reserves 67 of them and the next
                // aligned offset a page can carry is a whole 64 KiB on. Anything less and two of
                // these would overlap.
                if (at > 0 && held[at].getHandle() == held[at - 1].getHandle())
                {
                    EXPECT_GE(held[at].getOffset(), held[at - 1].getOffset() + coarse)
                        << "range " << at << " overlaps the one before it";
                }
            }
        }

        /// Two host-visible ranges are two windows on one mapping, and neither reaches the other.
        ///
        /// **A block is mapped once and a range is that pointer plus its offset**, which is how a
        /// caller reaches its memory — and the way to get it wrong is to hand every range the block's
        /// base.
        TEST_F(RtxMemoryTest, twoHostVisibleRangesAreSeparateWindowsOnOneMapping)
        {
            MemoryAllocator& memory = getDevice().getMemory();
            constexpr VkMemoryPropertyFlags staging
                = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

            const DeviceMemory first = memory.take(asks(1024, 256), staging, Tiling::Linear);
            const DeviceMemory second = memory.take(asks(1024, 256), staging, Tiling::Linear);

            ASSERT_NE(first.map(), nullptr);
            ASSERT_NE(second.map(), nullptr);
            ASSERT_NE(first.map(), second.map()) << "two ranges were handed the same address";

            auto* const one = static_cast<std::uint8_t*>(first.map());
            auto* const other = static_cast<std::uint8_t*>(second.map());

            for (int at = 0; at < 1024; ++at)
            {
                one[at] = 0x11;
                other[at] = 0x22;
            }

            EXPECT_EQ(one[0], 0x11) << "the second range wrote over the first";
            EXPECT_EQ(one[1023], 0x11) << "the second range wrote over the end of the first";
            EXPECT_EQ(other[0], 0x22);

            // And the address is the block's own plus the range's offset, which is what the caller
            // is promised: the two differ by exactly the difference of their offsets.
            if (first.getHandle() == second.getHandle())
            {
                EXPECT_EQ(other - one, static_cast<std::ptrdiff_t>(second.getOffset() - first.getOffset()))
                    << "a range's address is not its block's plus its offset";
            }
        }
    }
}
