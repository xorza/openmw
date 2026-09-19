#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <vulkan/vulkan_core.h>

#include <components/rtx/memoryreport.hpp>
#include <components/rtxvulkan/device.hpp>
#include <components/rtxvulkan/memory.hpp>
#include <components/rtxvulkan/owned.hpp>
#include <components/rtxvulkan/physicaldevice.hpp>
#include <components/rtxvulkan/result.hpp>

#include "harness.hpp"

namespace Rtx
{
    namespace
    {
        struct RtxMemoryTest : Testing::DeviceTest
        {
        };

        /// A buffer of `size` bytes and the room the allocator gave it, held together so the room
        /// outlives what is bound to it. The allocator is asked for a real buffer's room, because
        /// that is the one way it is asked and what it is told about the buffer — a linear resource
        /// of this size — is what decides where the range lands.
        struct Bound
        {
            Owned<VkBuffer, vkDestroyBuffer> mBuffer;
            DeviceMemory mMemory;
        };

        Bound bind(const Device& device, const VkDeviceSize size, const VkMemoryPropertyFlags properties)
        {
            const VkBufferCreateInfo create{
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .size = size,
                .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            };

            Bound bound;
            bound.mBuffer
                = Owned<VkBuffer, vkDestroyBuffer>::make(device.getHandle(), vkCreateBuffer, create, "vkCreateBuffer");
            bound.mMemory = device.getMemory().take(bound.mBuffer.get(), properties, 1);
            checkVk(vkBindBufferMemory(
                        device.getHandle(), bound.mBuffer.get(), bound.mMemory.getHandle(), bound.mMemory.getOffset()),
                "vkBindBufferMemory");

            return bound;
        }

        /// A thousand small resources come out of one allocation rather than a thousand.
        ///
        /// **What the allocator exists for.** A cell of Morrowind brings a few hundred textures and
        /// each carries a shading map of two kilobytes, so an allocation apiece is four figures of
        /// kernel-visible calls for the cell the game starts in, each for two kilobytes the driver
        /// rounds up to its own granularity. Two megabytes fit inside one block, so the honest claim
        /// is one and the assertion allows no more.
        TEST_F(RtxMemoryTest, aThousandSmallResourcesComeOutOfOneAllocation)
        {
            MemoryAllocator& memory = getDevice().getMemory();
            const std::size_t before = memory.getBlockCount();

            std::vector<Bound> held;
            held.reserve(1000);
            for (int at = 0; at < 1000; ++at)
                held.push_back(bind(getDevice(), 2048, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));

            // At most one, and not exactly one: a test that ran before this may have left a block
            // with room in it, and taking that room is the allocator working.
            EXPECT_LE(memory.getBlockCount() - before, 1u)
                << "a thousand small resources made more than one allocation";

            // And they are a thousand distinct places in it: no two ranges of one block overlap.
            for (std::size_t at = 1; at < held.size(); ++at)
            {
                if (held[at].mMemory.getHandle() != held[at - 1].mMemory.getHandle())
                    continue;

                EXPECT_GE(held[at].mMemory.getOffset(), held[at - 1].mMemory.getOffset() + 2048)
                    << "range " << at << " overlaps the one before it";
            }
        }

        /// A range given back is what the next resource of that size takes.
        ///
        /// **What makes a cell leaving pay for the cell arriving.** The hole a departing texture
        /// leaves is the hole the next one lands in — no call into the driver, and no block that
        /// grows for ever.
        TEST_F(RtxMemoryTest, aRangeGivenBackIsTakenOverByTheNextResource)
        {
            MemoryAllocator& memory = getDevice().getMemory();

            VkDeviceMemory handle = VK_NULL_HANDLE;
            VkDeviceSize offset = 0;
            {
                const Bound first = bind(getDevice(), 2048, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                handle = first.mMemory.getHandle();
                offset = first.mMemory.getOffset();
            }

            const std::size_t after = memory.getBlockCount();
            const Bound second = bind(getDevice(), 2048, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

            EXPECT_EQ(second.mMemory.getHandle(), handle) << "the range came from a different allocation";
            EXPECT_EQ(second.mMemory.getOffset(), offset) << "the range that was given back was not taken over";
            EXPECT_EQ(memory.getBlockCount(), after) << "taking over a hole made a new allocation";
        }

        /// Two host-visible ranges are two windows on one mapping, and neither reaches the other.
        ///
        /// **A range is mapped as it is made and the pointer is its own**, which is how a caller
        /// reaches its memory — and the way to get it wrong is to hand every range the block's base.
        TEST_F(RtxMemoryTest, twoHostVisibleRangesAreSeparateWindowsOnOneMapping)
        {
            constexpr VkMemoryPropertyFlags staging
                = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

            const Bound first = bind(getDevice(), 1024, staging);
            const Bound second = bind(getDevice(), 1024, staging);

            ASSERT_NE(first.mMemory.map(), nullptr);
            ASSERT_NE(second.mMemory.map(), nullptr);
            ASSERT_NE(first.mMemory.map(), second.mMemory.map()) << "two ranges were handed the same address";

            auto* const one = static_cast<std::uint8_t*>(first.mMemory.map());
            auto* const other = static_cast<std::uint8_t*>(second.mMemory.map());

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
            if (first.mMemory.getHandle() == second.mMemory.getHandle())
            {
                EXPECT_EQ(
                    other - one, static_cast<std::ptrdiff_t>(second.mMemory.getOffset() - first.mMemory.getOffset()))
                    << "a range's address is not its block's plus its offset";
            }
        }

        /// The report accounts for every block, and a range taken moves the live figure and not the
        /// reserved one.
        ///
        /// **The figure has to be real before anything can be decided on it.** A card whose
        /// host-visible heap is a couple of hundred megabytes fails on a number nothing in this
        /// renderer could state, so this is what says the number means something: reserved is what
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
            const Bound held = bind(getDevice(), wanted, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

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

        /// Blocks that empty go back to the device.
        ///
        /// **What a load's staging costs after the load.** A world's geometry goes to the device
        /// through 184 MiB of staging blocks, measured at Seyda Neen, and nothing reads them again —
        /// so a block that empties goes back to the device rather than standing until the renderer
        /// closes.
        TEST_F(RtxMemoryTest, blocksThatEmptyGoBackToTheDevice)
        {
            MemoryAllocator& memory = getDevice().getMemory();
            constexpr VkMemoryPropertyFlags staging
                = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

            const std::size_t before = memory.getBlockCount();

            // Larger than a block, so each range takes an allocation of its own whatever the tests
            // before this left standing in the shared device.
            constexpr VkDeviceSize alone = 96 * 1024 * 1024;

            std::vector<Bound> held;
            for (int at = 0; at < 3; ++at)
                held.push_back(bind(getDevice(), alone, staging));

            const std::size_t grown = memory.getBlockCount();
            ASSERT_EQ(grown, before + 3) << "three ranges too large to share did not make three blocks";

            held.clear();

            EXPECT_LT(memory.getBlockCount(), grown) << "emptied blocks were kept";
        }

        /// A budget the driver would not state is left out of the line rather than printed as none.
        ///
        /// **Zero and "would not say" are different answers**, and a reader who cannot tell them
        /// apart reads a card with no budget extension as a card with no memory left.
        TEST(RtxMemoryReportTest, aHeapWithNoBudgetLeavesTheBudgetColumnsOut)
        {
            // A card without resizable BAR: video memory, the system's, and the window.
            MemoryReport report;
            report.mHeapCount = 3;
            report.mHeaps[0] = HeapUse{ .mSize = 8ull << 30,
                .mReserved = 512ull << 20,
                .mLive = 256ull << 20,
                .mBlocks = 8,
                .mDeviceLocal = true };
            report.mHeaps[1] = HeapUse{ .mSize = 32ull << 30 };
            report.mHeaps[2] = HeapUse{ .mSize = 256ull << 20,
                .mBudget = 240ull << 20,
                .mHeld = 100ull << 20,
                .mReserved = 128ull << 20,
                .mLive = 120ull << 20,
                .mBlocks = 4,
                .mDeviceLocal = true,
                .mHostVisible = true };

            report.mHostWrittenReserved = 128ull << 20;
            report.mHostWrittenLive = 120ull << 20;

            const std::string out = describeMemory(report);

            EXPECT_EQ(out.find("budget"), out.rfind("budget")) << "the heap with no budget printed one";

            // **The line that answers the Turing question**, and the only one that can on a card
            // whose one video memory heap is host-visible throughout.
            EXPECT_NE(out.find("host-written"), std::string::npos);
            EXPECT_EQ(std::count(out.begin(), out.end(), '\n'), 4) << "a line a heap, and the one below them";
            EXPECT_NE(out.find("device-only"), std::string::npos);
            EXPECT_NE(out.find("system"), std::string::npos) << "the system's heap read as video memory";
            EXPECT_NE(out.find("host-visible"), std::string::npos);
            EXPECT_NE(out.find("8192.0 MiB"), std::string::npos) << "the first heap's size";
            EXPECT_NE(out.find("256.0 MiB"), std::string::npos) << "the window's size";
            EXPECT_EQ(out.substr(0, 2), "  ") << "the lines were not indented as asked";
        }
    }
}
