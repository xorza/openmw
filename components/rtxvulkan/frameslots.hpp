#pragma once

#include <cstdint>
#include <span>

#include <components/rtx/index.hpp>
#include <components/rtx/slotset.hpp>

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
    class RowDebt
    {
    public:
        /// Names `rows`, each of them once however often it is named.
        void owe(std::span<const Index> rows)
        {
            if (mEverything)
                return;

            for (const Index at : rows)
                mRows.addMakingRoom(at);
        }

        void oweEverything() { mEverything = true; }

        bool owesEverything() const { return mEverything; }
        bool owesAnything() const { return mEverything || !mRows.empty(); }

        std::span<const Index> getRows() const { return mRows.getSlots(); }

        void settle()
        {
            mEverything = false;
            mRows.clear();
        }

    private:
        bool mEverything = true;

        /// The rows this copy is behind on, each named once however often it is written.
        ///
        /// **A set and not a vector**, because a value settled in two steps writes its row twice
        /// before either copy is paid, and a debt that searched itself on every write would cost
        /// the square of the rows a frame touches.
        SlotSet mRows;
    };
}
