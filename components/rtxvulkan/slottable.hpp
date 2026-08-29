#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <vulkan/vulkan_core.h>

#include <components/rtx/scenedesc.hpp>

#include "device.hpp"
#include "frameslots.hpp"
#include "graveyard.hpp"
#include "hostbuffer.hpp"

namespace Rtx
{
    /// One host-side table and one device copy of it per frame in flight.
    ///
    /// **A copy is behind because something wrote a row, and for no other reason.** That sentence is
    /// the whole of what this type exists to make true. The mechanism it replaces derived each
    /// copy's debt from the scene's own change lists — `getMoved`, `getSettled`, `getDeformed` —
    /// replayed at five separate sites, and correctness then rested on four things that nothing
    /// checked: that every site subscribed to the same lists, that the lists were still current when
    /// each site ran, that every writer settled exactly once, and that a table's growth path agreed
    /// with its debt about when a copy had been filled whole. Each of those failed at least once,
    /// silently, and the symptom was a frame of wrong geometry. `.notes/rtx/slot-tables.md` is the
    /// account.
    ///
    /// Here there is nothing to subscribe to and no list whose lifetime matters. `write` marks the
    /// row owed by every copy; `sync` pays one copy's debt and clears it, in one loop, in one place.
    ///
    /// **The host rows are the truth and the copies are derived from them.** So a row is computed
    /// once however many copies take it, and a copy filled whole and a copy given three rows are the
    /// same code reading the same array.
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

        /// The host-side rows, which is what every copy is written from.
        std::span<const Row> getRows() const { return mRows; }

        /// The row at `at`, to be written. **Owed by every copy from here on**, including the one
        /// about to be synced: a caller that writes a row and syncs is a caller whose copy has it.
        Row& write(Index at)
        {
            assert(at < mRows.size() && "a row past the end of the table; grow it first");

            for (std::uint32_t slot = 0; slot < mSlots; ++slot)
                mOwed[slot].owe(std::span<const Index>(&at, 1));

            return mRows[at];
        }

        /// Makes the table `rows` long, value-initialising anything appended.
        ///
        /// **What is appended is owed and what was already there is not.** A row keeps its offset
        /// when the table grows, so a copy that has row seven still has it; only the rows past its
        /// old end are news to it. A copy whose *buffer* has to be made again is a different
        /// question, and `sync` is where that one is asked.
        void resize(std::size_t rows)
        {
            const std::size_t had = mRows.size();
            if (rows == had)
                return;

            mRows.resize(rows);
            if (rows < had)
                return;

            mAppended.clear();
            mAppended.reserve(rows - had);
            for (std::size_t at = had; at < rows; ++at)
                mAppended.push_back(static_cast<Index>(at));

            for (std::uint32_t slot = 0; slot < mSlots; ++slot)
                mOwed[slot].owe(mAppended);
        }

        /// Whether `slot`'s copy would change if it were synced now.
        ///
        /// **What an early return asks.** A caller that skips work when nothing has changed has to
        /// ask the copy it would have written, not the scene it would have read: a copy can carry a
        /// debt from frames ago while the scene stands perfectly still.
        bool owes(std::uint32_t slot) const
        {
            assert(slot < mSlots);
            return mOwed[slot].mEverything || !mOwed[slot].mRows.empty();
        }

        /// Writes what `slot`'s copy owes and clears the debt.
        ///
        /// @param graveyard takes the buffer a growth displaced, which a frame in flight may still
        ///        be reading.
        void sync(std::uint32_t slot, Graveyard& graveyard)
        {
            assert(slot < mSlots);
            assert(mDevice != nullptr && "sync before open");

            HostBuffer& copy = mCopies[slot];
            RowDebt& owed = mOwed[slot];
            const VkDeviceSize needed = mRows.size() * sizeof(Row);

            // **A copy made again is empty whatever the debt says**, so a growth that reallocates
            // is itself a reason to write the whole table. That agreement between growth and debt
            // used to be spelled out once per table, differently each time.
            //
            // **Asked only where it does not fit**, and doubled when it is asked, so a table that
            // keeps growing is made again a logarithmic number of times rather than once an arrival.
            // Doubling unconditionally is doubling every frame: `growTo` remakes whatever it is
            // asked for that is larger than what it has, and twice the size always is.
            //
            // A byte where the table is empty, because a descriptor with nothing bound to it is
            // undefined rather than blank. `growTo` is where that rule lives.
            const VkDeviceSize least = std::max(needed, VkDeviceSize{ 1 });
            if (copy.getSize() < least)
            {
                graveyard.bury(growTo(copy, *mDevice, std::max(least, copy.getSize() * 2), mUsage));
                mDevice->setName(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<std::uint64_t>(copy.getHandle()), mName);
                owed.mEverything = true;
            }

            if (owed.mEverything)
                copy.write(std::span<const Row>(mRows));
            else
                for (const Index at : owed.mRows)
                    copy.writeAt(at * sizeof(Row), std::span<const Row>(&mRows[at], 1));

            owed.settle();
        }

        VkBuffer getHandle(std::uint32_t slot) const
        {
            assert(slot < mSlots);
            return mCopies[slot].getHandle();
        }

        VkDeviceAddress getDeviceAddress(std::uint32_t slot) const
        {
            assert(slot < mSlots);
            return mCopies[slot].getDeviceAddress();
        }

        /// What one copy's buffer occupies, for a test that asks whether it keeps growing.
        VkDeviceSize getCopyBytes(std::uint32_t slot) const
        {
            assert(slot < mSlots);
            return mCopies[slot].getSize();
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
        std::span<const Index> getOwed(std::uint32_t slot) const
        {
            assert(slot < mSlots);
            return mOwed[slot].mRows;
        }

        bool owesEverything(std::uint32_t slot) const
        {
            assert(slot < mSlots);
            return mOwed[slot].mEverything;
        }

    private:
        const Device* mDevice = nullptr;
        std::uint32_t mSlots = 1;
        VkBufferUsageFlags mUsage = 0;
        std::string mName;

        std::vector<Row> mRows;
        std::array<HostBuffer, sFrameSlots> mCopies;
        std::array<RowDebt, sFrameSlots> mOwed;

        /// Cleared and refilled by `resize`, never freed: the rows one growth appended.
        std::vector<Index> mAppended;
    };
}
