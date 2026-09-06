#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtx/memoryreport.hpp>
#include <components/rtxvulkan/device.hpp>
#include <components/rtxvulkan/memory.hpp>
#include <components/rtxvulkan/physicaldevice.hpp>

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

        /// The report accounts for every block, and a range taken moves the live figure and not the
        /// reserved one.
        ///
        /// **What Stage 0 of `.notes/design-vulkan.md` exists for.** A card whose host-visible heap
        /// is a couple of hundred megabytes fails on a figure nothing in this renderer could state,
        /// so the first thing to get right is that the figure is real: reserved is what
        /// `vkAllocateMemory` asked for, live is what is inside it, and one never exceeds the other.
        TEST_F(RtxMemoryTest, theReportCountsWhatWasReservedAndWhatIsLive)
        {
            MemoryAllocator& memory = getDevice().getMemory();

            const MemoryReport before = memory.report();
            ASSERT_GT(before.mHeapCount, 0u) << "a device with no memory heaps";

            const auto live = [](const MemoryReport& report) {
                std::uint64_t total = 0;
                for (std::uint32_t heap = 0; heap < report.mHeapCount; ++heap)
                    total += report.mHeaps[heap].mLive;

                return total;
            };

            // Half a block, so the range cannot come out of a hole an earlier test left and cannot
            // help but be visible in the live figure.
            constexpr VkDeviceSize wanted = 4 * 1024 * 1024;
            const DeviceMemory held
                = memory.take(asks(wanted, 1024), VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, Tiling::Optimal);

            const MemoryReport during = memory.report();
            EXPECT_EQ(during.mHeapCount, before.mHeapCount);
            EXPECT_GE(live(during), live(before) + wanted)
                << "a range of " << wanted << " bytes did not show in the live figure";

            for (std::uint32_t heap = 0; heap < during.mHeapCount; ++heap)
            {
                const HeapUse& use = during.mHeaps[heap];
                EXPECT_LE(use.mLive, use.mReserved) << "heap " << heap << " holds more than it reserved";
                EXPECT_LE(use.mReserved, use.mSize) << "heap " << heap << " reserved more than it has";
                EXPECT_EQ(use.mBlocks == 0, use.mReserved == 0) << "heap " << heap << " counted blocks and no bytes";
            }

            std::uint32_t blocks = 0;
            for (std::uint32_t heap = 0; heap < during.mHeapCount; ++heap)
                blocks += during.mHeaps[heap].mBlocks;
            EXPECT_EQ(blocks, memory.getBlockCount()) << "the report left a block out";
        }

        /// A heap is called host-visible in the report when it carries a type `Buffer::hostWritten`
        /// could take a range out of, and not otherwise.
        ///
        /// **The question a Turing card fails.** Its host-visible video memory heap is 246 MiB
        /// beside six gigabytes of ordinary video memory, so which heap is which is the whole of
        /// what a residency decision reads.
        TEST_F(RtxMemoryTest, aHostVisibleHeapIsTheOneAHostWrittenBufferCouldComeOutOf)
        {
            const VkPhysicalDeviceMemoryProperties& properties
                = getDevice().getPhysicalDevice().getProperties().mMemory;

            std::vector<bool> expected(properties.memoryHeapCount, false);
            for (std::uint32_t type = 0; type < properties.memoryTypeCount; ++type)
                if ((properties.memoryTypes[type].propertyFlags & sHostWritten) == sHostWritten)
                    expected[properties.memoryTypes[type].heapIndex] = true;

            const MemoryReport report = getDevice().getMemory().report();
            ASSERT_EQ(report.mHeapCount, properties.memoryHeapCount);
            for (std::uint32_t heap = 0; heap < report.mHeapCount; ++heap)
                EXPECT_EQ(report.mHeaps[heap].mHostVisible, expected[heap]) << "heap " << heap;

            // The renderer requires such a heap, so a device that reached here has one.
            EXPECT_NE(std::find(expected.begin(), expected.end(), true), expected.end())
                << "no heap the host writes into, on a device this renderer accepted";
        }

        /// A budget the driver would not state is left out of the line rather than printed as none.
        ///
        /// **Zero and "would not say" are different answers**, and a reader who cannot tell them
        /// apart reads a card with no budget extension as a card with no memory left.
        TEST(RtxMemoryReportTest, aHeapWithNoBudgetLeavesTheBudgetColumnsOut)
        {
            MemoryReport report;
            report.mHeapCount = 2;
            report.mHeaps[0]
                = HeapUse{ .mSize = 8ull << 30, .mReserved = 512ull << 20, .mLive = 256ull << 20, .mBlocks = 8 };
            report.mHeaps[1] = HeapUse{ .mSize = 256ull << 20,
                .mBudget = 240ull << 20,
                .mHeld = 100ull << 20,
                .mReserved = 128ull << 20,
                .mLive = 120ull << 20,
                .mBlocks = 4,
                .mHostVisible = true };

            report.mHostWrittenReserved = 128ull << 20;
            report.mHostWrittenLive = 120ull << 20;

            const std::string out = describeMemory(report);

            EXPECT_EQ(out.find("budget"), out.rfind("budget")) << "the heap with no budget printed one";

            // **The line that answers the Turing question**, and the only one that can on a card
            // whose one video memory heap is host-visible throughout.
            EXPECT_NE(out.find("host-written"), std::string::npos);
            EXPECT_EQ(std::count(out.begin(), out.end(), '\n'), 3) << "a line a heap, and the one below them";
            EXPECT_NE(out.find("device-only"), std::string::npos);
            EXPECT_NE(out.find("host-visible"), std::string::npos);
            EXPECT_NE(out.find("8192.0 MiB"), std::string::npos) << "the first heap's size";
            EXPECT_NE(out.find("256.0 MiB"), std::string::npos) << "the second heap's size";
            EXPECT_EQ(out.substr(0, 2), "  ") << "the lines were not indented as asked";
        }
    }
}
