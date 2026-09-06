#include "memoryreport.hpp"

#include <format>

namespace Rtx
{
    namespace
    {
        double megabytes(std::uint64_t bytes)
        {
            return static_cast<double>(bytes) / (1024.0 * 1024.0);
        }

        /// How far into a heap's line `reserved` begins.
        ///
        /// The line under the heaps is padded to it so the two figures read down the page, which
        /// ties the two formats below together — named rather than counted a second time.
        constexpr int sReservedColumn = 37;
    }

    std::string describeMemory(const MemoryReport& report)
    {
        std::string out;
        for (std::uint32_t heap = 0; heap < report.mHeapCount; ++heap)
        {
            const HeapUse& use = report.mHeaps[heap];

            // **The host-visible one is named, because it is the one that runs out.** A card without
            // resizable BAR states it at a couple of hundred megabytes beside a video memory heap
            // of gigabytes, and a reader scanning these lines has to be able to tell which is which
            // without adding the flags up.
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
