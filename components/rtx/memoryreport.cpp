#include "memoryreport.hpp"

#include <format>

namespace Rtx
{
    namespace
    {
        /// How far into a heap's line `reserved` begins, so the line under the heaps reads down
        /// the page with it.
        constexpr int sReservedColumn = 37;
    }

    std::string describeMemory(const MemoryReport& report)
    {
        std::string out;
        for (std::uint32_t heap = 0; heap < report.mHeapCount; ++heap)
        {
            const HeapUse& use = report.mHeaps[heap];

            // The host-visible one is named, because it is the one that runs out on a card without
            // resizable BAR.
            out += std::format("  heap {}  {:<12}  {:8.1f} MiB   reserved {:7.1f}   live {:7.1f}", heap,
                use.mHostVisible ? "host-visible" : "device-only", megabytes(use.mSize), megabytes(use.mReserved),
                megabytes(use.mLive));

            // Nought from a driver that would not say, which is not the same answer as a budget of
            // none — so the columns are left off rather than printed as zeroes.
            if (use.mBudget > 0)
                out += std::format("   budget {:7.1f}   held {:7.1f}", megabytes(use.mBudget), megabytes(use.mHeld));

            out += std::format("   {} blocks\n", use.mBlocks);
        }

        // **Under the heaps and in their columns, because it cuts across them.** A card with
        // resizable BAR states one heap that is host-visible throughout, so this is the only line
        // that says what would have to fit in the small aperture of a card without it.
        out += std::format("  {:<{}}reserved {:7.1f}   live {:7.1f}\n", "host-written", sReservedColumn,
            megabytes(report.mHostWrittenReserved), megabytes(report.mHostWrittenLive));

        return out;
    }
}
