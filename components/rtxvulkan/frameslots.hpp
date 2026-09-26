#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>

#include <components/rtx/runs.hpp>
#include <components/rtx/slots.hpp>

namespace Rtx
{
    /// How many frames may be in flight over one scene at once, which is how many copies there are
    /// of every table a frame writes. Two, because the CPU is one frame ahead of the GPU and no
    /// more: the walk and the placement of frame N+1 run while frame N is traced, so the tables N+1
    /// writes cannot be the ones N reads, and a third copy would buy nothing, since the CPU has
    /// nothing to do that far ahead.
    inline constexpr std::uint32_t sFrameSlots = 2;

    /// Which copy of a double-buffered table a frame writes. A type and not a `std::uint32_t`,
    /// because a scene slot, a GUI texture and this were one spelling between them — and
    /// `traceGuiTexture` chose between a scene's and a frame's in one expression.
    class FrameSlot
    {
    public:
        constexpr FrameSlot() = default;

        constexpr explicit FrameSlot(std::uint32_t index)
            : mIndex(index)
        {
            assert(index < sFrameSlots && "a frame slot past the copies there are");
        }

        constexpr std::uint32_t get() const { return mIndex; }

        constexpr FrameSlot next() const { return FrameSlot{ (mIndex + 1) % sFrameSlots }; }

        constexpr bool operator==(const FrameSlot& other) const = default;

    private:
        std::uint32_t mIndex = 0;
    };

    /// One `T` per frame in flight, addressed by `FrameSlot`: the copies of every table a frame
    /// writes. Room for `sFrameSlots` and `count()` of them live, so a scene told to keep fewer
    /// copies keeps fewer — and the one assert on a slot lives here.
    template <class T>
    class PerSlot
    {
    public:
        /// `sFrameSlots` default-made, every one live, for a member that is `open`ed later.
        PerSlot() = default;

        /// One per slot, each `make(slot)`, every one live: for a `T` that has no empty state.
        template <class Make, class = std::enable_if_t<std::is_invocable_v<Make&, FrameSlot>>>
        explicit PerSlot(Make&& make)
            : PerSlot(make, std::make_index_sequence<sFrameSlots>{})
        {
        }

        /// Says how many of them a scene keeps: at least one, at most all of them.
        void open(const std::uint32_t count)
        {
            assert(count >= 1 && count <= sFrameSlots && "more frames in flight than there are copies");
            mCount = count;
        }

        std::uint32_t count() const { return mCount; }

        T& at(const FrameSlot slot)
        {
            assert(slot.get() < mCount && "a frame slot this keeps no copy for");
            return mItems[slot.get()];
        }

        const T& at(const FrameSlot slot) const
        {
            assert(slot.get() < mCount && "a frame slot this keeps no copy for");
            return mItems[slot.get()];
        }

        /// The live ones, for a write that every copy owes.
        std::span<T> live() { return std::span<T>(mItems.data(), mCount); }
        std::span<const T> live() const { return std::span<const T>(mItems.data(), mCount); }

    private:
        template <class Make, std::size_t... I>
        PerSlot(Make& make, std::index_sequence<I...>)
            : mItems{ make(FrameSlot{ static_cast<std::uint32_t>(I) })... }
        {
        }

        std::array<T, sFrameSlots> mItems{};
        std::uint32_t mCount = sFrameSlots;
    };

    /// What one copy of a double-buffered table still has to be told. Two frames in flight is two
    /// copies of every table a frame writes, and the copy the frame before last wrote is two frames
    /// behind: it owes the rows that frame changed and the rows this one does. The debt is those
    /// rows — or everything, where the copy was just made — and writing the copy is what settles
    /// it. A frame owes its rows to every copy and settles the one it uses.
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

        /// The rows this copy is behind on, each named once however often it is written. A set and
        /// not a vector, because a value settled in two steps writes its row twice before either
        /// copy is paid, and a debt that searched itself on every write would cost the square of
        /// the rows a frame touches.
        SlotSet mRows;
    };
}
