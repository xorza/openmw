#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace Rtx
{
    /// `bytes` in mebibytes, which is the unit every report in this fork prints memory in.
    inline double megabytes(std::uint64_t bytes)
    {
        return static_cast<double>(bytes) / (1024.0 * 1024.0);
    }

    /// One of the device's memory heaps, and what this renderer has taken out of it.
    struct HeapUse
    {
        /// The heap itself, as the device states it.
        std::uint64_t mSize = 0;

        /// What the driver says this process may have of it, and what it says the process already
        /// holds — both nought where the driver will not say.
        ///
        /// **A budget is not the heap.** It moves with what else is running, and it is the figure a
        /// residency decision is made against; the heap's size is only its ceiling.
        std::uint64_t mBudget = 0;
        std::uint64_t mHeld = 0;

        /// What the renderer's allocator asked this heap for, and what the resources inside those
        /// allocations occupy.
        ///
        /// **The gap between them is the price of suballocating**, and it is the figure that says
        /// whether a block ought to be given back rather than kept.
        std::uint64_t mReserved = 0;
        std::uint64_t mLive = 0;

        std::uint32_t mBlocks = 0;

        /// Whether the device reads this heap and the host writes into it directly.
        ///
        /// **The one heap a card without resizable BAR keeps small** — a couple of hundred
        /// megabytes — and so the one a run has to be measured against before it is called
        /// portable.
        bool mHostVisible = false;
    };

    /// Every heap of the device the renderer is running on.
    struct MemoryReport
    {
        /// **Fixed, so that a report can be copied into a bench record without an allocation.** No
        /// device this renderer targets states more heaps than this; a device that stated more
        /// would have the rest left out rather than counted wrongly.
        static constexpr std::size_t sMaxHeaps = 16;

        std::array<HeapUse, sMaxHeaps> mHeaps{};
        std::uint32_t mHeapCount = 0;

        /// What the renderer put in memory the host writes into and the device reads.
        ///
        /// **The figure a card without resizable BAR runs out of, and one no heap can state.** This
        /// box's video memory is a single 16 GiB heap that is host-visible throughout, so every
        /// image lands in the same heap as the geometry and the heap's own line says nothing about
        /// either. A Turing card keeps that memory in a heap of its own of a couple of hundred
        /// megabytes, and what has to fit there is exactly this — which is a property of the memory
        /// type asked for, not of the heap it came out of.
        std::uint64_t mHostWrittenReserved = 0;
        std::uint64_t mHostWrittenLive = 0;
    };

    /// The report as the harness prints it, one line a heap, indented to sit under the place it
    /// belongs to.
    std::string describeMemory(const MemoryReport& report);
}
