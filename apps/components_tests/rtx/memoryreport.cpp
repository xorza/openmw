#include <algorithm>
#include <string>

#include <gtest/gtest.h>

#include <components/rtx/memoryreport.hpp>

namespace Rtx
{
    namespace
    {
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
