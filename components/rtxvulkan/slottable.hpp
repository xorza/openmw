#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <vulkan/vulkan_core.h>

#include <components/rtx/runs.hpp>
#include <components/rtx/slots.hpp>

#include "blockedbuffer.hpp"
#include "device.hpp"
#include "frameslots.hpp"
#include "graveyard.hpp"

namespace Rtx
{
    /// One host-side table and one device copy of it per frame in flight. A copy is behind because
    /// something wrote a row, and for no other reason: `write` marks the row owed by every copy and
    /// `sync` pays one copy's debt, in one place, where a debt derived from the scene's change
    /// lists replayed at every table failed silently as a frame of wrong geometry. The host rows
    /// are the truth, so a row is computed once however many copies take it.
    template <class Row>
    class SlotTable
    {
    public:
        /// @param slots how many frames may be in flight, and so how many copies there are.
        /// @param usage what the device does with the copies.
        void open(const Device& device, std::uint32_t slots, VkBufferUsageFlags usage, std::string_view name)
        {
            assert(slots >= 1 && slots <= sFrameSlots && "more frames in flight than there are copies");
            mDevice = &device;
            mSlots = slots;
            mUsage = usage;
            mName = name;
        }

        std::size_t size() const { return mRows.size(); }

        std::span<const Row> getRows() const { return mRows; }

        /// The row at `at`, to be written. Owed by every copy from here on, including the one
        /// about to be synced: a caller that writes a row and syncs is a caller whose copy has it.
        Row& write(Index at)
        {
            assert(at < mRows.size() && "a row past the end of the table; grow it first");

            for (std::uint32_t slot = 0; slot < mSlots; ++slot)
                mOwed[slot].owe(std::span<const Index>(&at, 1));

            return mRows[at];
        }

        /// Makes the table `rows` long, value-initialising anything appended. What is appended is
        /// owed and what was already there is not; what is dropped is forgotten, or a copy still
        /// owing a row past the new end would send `sync` indexing past `mRows`.
        void resize(std::size_t rows)
        {
            const std::size_t had = mRows.size();
            if (rows == had)
                return;

            mRows.resize(rows);
            if (rows < had)
            {
                for (std::uint32_t slot = 0; slot < mSlots; ++slot)
                    mOwed[slot].shrinkTo(rows);

                return;
            }

            mAppended.clear();
            mAppended.reserve(rows - had);
            for (std::size_t at = had; at < rows; ++at)
                mAppended.push_back(static_cast<Index>(at));

            for (std::uint32_t slot = 0; slot < mSlots; ++slot)
                mOwed[slot].owe(mAppended);
        }

        /// Whether `slot`'s copy would change if it were synced now — what an early return asks,
        /// because a copy can carry a debt from frames ago while the scene stands still.
        bool owes(FrameSlot slot) const
        {
            assert(slot.get() < mSlots);
            return mOwed[slot.get()].owesAnything();
        }

        /// Writes what `slot`'s copy owes and clears the debt.
        ///
        /// @param graveyard takes the buffer a growth displaced, which a frame in flight may still
        ///        be reading.
        void sync(FrameSlot slot, Graveyard& graveyard)
        {
            assert(slot.get() < mSlots);
            assert(mDevice != nullptr && "sync before open");

            Buffer& copy = mCopies[slot.get()];
            RowDebt& owed = mOwed[slot.get()];
            const VkDeviceSize needed = mRows.size() * sizeof(Row);

            // A copy made again is empty whatever the debt says. Doubled only where it does not
            // fit, because `growTo` remakes whatever is larger than what it has. A byte where the
            // table is empty, because a descriptor with nothing bound is undefined rather than blank.
            const VkDeviceSize least = std::max(needed, VkDeviceSize{ 1 });
            if (copy.getSize() < least)
            {
                graveyard.bury(growTo(copy, *mDevice, std::max(least, copy.getSize() * 2), mUsage));
                mDevice->setName(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<std::uint64_t>(copy.getHandle()), mName);
                owed.oweEverything();
            }

            if (owed.owesEverything())
                copy.write(std::span<const Row>(mRows));
            else
                for (const Index at : owed.getRows())
                {
                    assert(at < mRows.size() && "a debt naming a row the table no longer has");
                    copy.writeAt(at * sizeof(Row), std::span<const Row>(&mRows[at], 1));
                }

            owed.settle();
        }

        VkDeviceAddress getDeviceAddress(FrameSlot slot) const
        {
            assert(slot.get() < mSlots);
            return mCopies[slot.get()].getDeviceAddress();
        }

        /// What one copy's buffer occupies, for a test that asks whether it keeps growing.
        VkDeviceSize getCopyBytes(FrameSlot slot) const
        {
            assert(slot.get() < mSlots);
            return mCopies[slot.get()].getSize();
        }

        VkDeviceSize getBytes() const
        {
            VkDeviceSize total = 0;
            for (std::uint32_t slot = 0; slot < mSlots; ++slot)
                total += mCopies[slot].getSize();

            return total;
        }

        /// What `slot` would write if it were synced now, for a test that asks whether the
        /// bookkeeping is right rather than whether the picture is.
        std::span<const Index> getOwed(FrameSlot slot) const
        {
            assert(slot.get() < mSlots);
            return mOwed[slot.get()].getRows();
        }

        bool owesEverything(FrameSlot slot) const
        {
            assert(slot.get() < mSlots);
            return mOwed[slot.get()].owesEverything();
        }

    private:
        const Device* mDevice = nullptr;
        std::uint32_t mSlots = 1;
        VkBufferUsageFlags mUsage = 0;
        /// A literal, which is what every caller passes and all a debug name is asked to be.
        std::string_view mName;

        std::vector<Row> mRows;
        std::array<Buffer, sFrameSlots> mCopies;
        std::array<RowDebt, sFrameSlots> mOwed;

        /// Cleared and refilled by `resize`, never freed: the rows one growth appended.
        std::vector<Index> mAppended;
    };

    /// One `BlockedBuffer` per frame in flight, and what each copy has yet to be told —
    /// `SlotTable`'s sibling for a table whose truth is the scene's, so the caller says how to read
    /// a run and this says which are owed.
    class SlotBlocks
    {
    public:
        SlotBlocks(std::uint32_t blockSize, std::uint32_t stride)
            : mCopies{ BlockedBuffer{ blockSize, stride }, BlockedBuffer{ blockSize, stride } }
        {
        }

        void open(const Device& device, std::uint32_t slots, VkBufferUsageFlags usage, std::string_view name)
        {
            assert(slots >= 1 && slots <= sFrameSlots && "more frames in flight than there are copies");
            mSlots = slots;
            for (std::uint32_t slot = 0; slot < mSlots; ++slot)
                mCopies[slot].open(device, usage, name);
        }

        /// Makes room in every copy for `elements`. Nothing already written moves, which is what a
        /// block table is for, so this owes nothing on its own.
        void reserve(Batch& batch, std::uint32_t elements)
        {
            for (std::uint32_t slot = 0; slot < mSlots; ++slot)
                mCopies[slot].reserve(batch, elements);
        }

        /// Says that `at`'s run has changed, so every copy owes it — once, however often it is
        /// named before that copy is filled.
        void write(Index at)
        {
            for (std::uint32_t slot = 0; slot < mSlots; ++slot)
                mOwed[slot].addMakingRoom(at);
        }

        void write(std::span<const Index> runs)
        {
            for (const Index at : runs)
                write(at);
        }

        /// Says that `slot`'s copy holds everything there is to hold, which is what a load ends
        /// with: a load writes every copy through `at` and this is what tells the account.
        void settle(FrameSlot slot)
        {
            assert(slot.get() < mSlots);
            mOwed[slot.get()].clear();
        }

        /// Writes the runs `slot`'s copy owes and clears the debt.
        ///
        /// @param fill `void(Index at, BlockedBuffer& into)`, which copies that one run in. Called
        ///        once per owed run and never for a run this copy already has.
        template <class Fill>
        void sync(FrameSlot slot, Fill&& fill)
        {
            assert(slot.get() < mSlots);
            for (const Index at : mOwed[slot.get()].getSlots())
                fill(at, mCopies[slot.get()]);

            mOwed[slot.get()].clear();
        }

        /// One copy, written or read behind the account's back, because an arrival fills every
        /// copy whole and then says so with `settle`. Per-frame writes go through `write` and
        /// `sync`.
        BlockedBuffer& at(FrameSlot slot)
        {
            assert(slot.get() < mSlots);
            return mCopies[slot.get()];
        }

        const BlockedBuffer& at(FrameSlot slot) const
        {
            assert(slot.get() < mSlots);
            return mCopies[slot.get()];
        }

        VkDeviceSize getBytes() const
        {
            VkDeviceSize total = 0;
            for (std::uint32_t slot = 0; slot < mSlots; ++slot)
                total += mCopies[slot].getBytes();

            return total;
        }

        std::span<const Index> getOwed(FrameSlot slot) const
        {
            assert(slot.get() < mSlots);
            return mOwed[slot.get()].getSlots();
        }

    private:
        std::uint32_t mSlots = 1;
        std::array<BlockedBuffer, sFrameSlots> mCopies;

        /// A set and not a `RowDebt`, because a block table's data is the scene's and there is no
        /// "everything" here to owe. Cleared and refilled, never freed.
        std::array<SlotSet, sFrameSlots> mOwed;
    };
}
