#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <components/rtx/scenedesc.hpp>

namespace Rtx
{
    /// How many frames may be in flight over one scene at once, which is how many copies there are
    /// of every table a frame writes.
    ///
    /// **Two, because the CPU is one frame ahead of the GPU and no more.** The walk and the
    /// placement of frame N+1 run while frame N is traced, so the tables N+1 writes cannot be the
    /// ones N reads; a third copy would buy nothing, since the CPU has nothing to do that far ahead.
    inline constexpr std::uint32_t sFrameSlots = 2;

    /// What one copy of a double-buffered table still has to be told.
    ///
    /// **Two frames in flight is two copies of every table a frame writes**, and the copy the frame
    /// before last wrote is two frames behind: it owes the rows that frame changed and the rows this
    /// one does. The debt is those rows — or everything, where the copy was just made — and writing
    /// the copy is what settles it. A frame owes its rows to every copy and settles the one it uses.
    struct RowDebt
    {
        bool mEverything = true;

        /// Cleared and refilled, never freed; it settles at the busiest pair of frames so far.
        std::vector<Index> mRows;

        void owe(std::span<const Index> rows)
        {
            if (!mEverything)
                mRows.insert(mRows.end(), rows.begin(), rows.end());
        }

        void settle()
        {
            mEverything = false;
            mRows.clear();
        }
    };
}
