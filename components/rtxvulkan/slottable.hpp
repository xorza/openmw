#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <vulkan/vulkan_core.h>

#include <components/rtx/scenedesc.hpp>

#include "blockedbuffer.hpp"
#include "device.hpp"
#include "frameslots.hpp"
#include "graveyard.hpp"

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
    /// silently, and the symptom was a frame of wrong geometry.
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
            return mOwed[slot].owesAnything();
        }

        /// Writes what `slot`'s copy owes and clears the debt.
        ///
        /// @param graveyard takes the buffer a growth displaced, which a frame in flight may still
        ///        be reading.
        void sync(std::uint32_t slot, Graveyard& graveyard)
        {
            assert(slot < mSlots);
            assert(mDevice != nullptr && "sync before open");

            Buffer& copy = mCopies[slot];
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
                owed.oweEverything();
            }

            if (owed.owesEverything())
                copy.write(std::span<const Row>(mRows));
            else
                for (const Index at : owed.getRows())
                    copy.writeAt(at * sizeof(Row), std::span<const Row>(&mRows[at], 1));

            owed.settle();
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
            return mOwed[slot].getRows();
        }

        bool owesEverything(std::uint32_t slot) const
        {
            assert(slot < mSlots);
            return mOwed[slot].owesEverything();
        }

    private:
        const Device* mDevice = nullptr;
        std::uint32_t mSlots = 1;
        VkBufferUsageFlags mUsage = 0;
        std::string mName;

        std::vector<Row> mRows;
        std::array<Buffer, sFrameSlots> mCopies;
        std::array<RowDebt, sFrameSlots> mOwed;

        /// Cleared and refilled by `resize`, never freed: the rows one growth appended.
        std::vector<Index> mAppended;
    };

    /// One `BlockedBuffer` per frame in flight, and what each copy has yet to be told.
    ///
    /// **`SlotTable`'s sibling for a table whose truth lives elsewhere.** A row table is written
    /// from host rows this object owns; a block table holds a mesh's vertices, and those are the
    /// scene's — so the caller says how to read one and this says which ones are owed. The rule
    /// either way is the same and it is the whole point of both: a copy is behind because `write`
    /// named it, and `sync` is the only thing that clears that.
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
        void reserve(std::uint32_t elements)
        {
            for (std::uint32_t slot = 0; slot < mSlots; ++slot)
                mCopies[slot].reserve(elements);
        }

        /// Says that `at`'s run has changed, so every copy owes it.
        void write(Index at)
        {
            for (std::uint32_t slot = 0; slot < mSlots; ++slot)
                mOwed[slot].push_back(at);
        }

        void write(std::span<const Index> runs)
        {
            for (std::uint32_t slot = 0; slot < mSlots; ++slot)
                mOwed[slot].insert(mOwed[slot].end(), runs.begin(), runs.end());
        }

        /// Says that `slot`'s copy holds everything there is to hold, which is what a load ends
        /// with: a load writes every copy through `at` and this is what tells the account.
        void settle(std::uint32_t slot)
        {
            assert(slot < mSlots);
            mOwed[slot].clear();
        }

        /// Writes the runs `slot`'s copy owes and clears the debt.
        ///
        /// @param fill `void(Index at, BlockedBuffer& into)`, which copies that one run in. Called
        ///        once per owed run and never for a run this copy already has.
        template <class Fill>
        void sync(std::uint32_t slot, Fill&& fill)
        {
            assert(slot < mSlots);
            for (const Index at : mOwed[slot])
                fill(at, mCopies[slot]);

            mOwed[slot].clear();
        }

        /// One copy, written or read behind the account's back.
        ///
        /// **The one way to break the rule this type is for**, and it is here because a load has to
        /// break it: an arrival fills every copy whole and then says so with `settle`, which is
        /// cheaper and clearer than naming every run it just wrote. A caller that writes through
        /// this and does not settle has left the account describing a copy that no longer matches
        /// it, which is the whole failure this replaced. Per-frame writes go through `write` and
        /// `sync`.
        BlockedBuffer& at(std::uint32_t slot)
        {
            assert(slot < mSlots);
            return mCopies[slot];
        }

        const BlockedBuffer& at(std::uint32_t slot) const
        {
            assert(slot < mSlots);
            return mCopies[slot];
        }

        VkDeviceSize getBytes() const
        {
            VkDeviceSize total = 0;
            for (std::uint32_t slot = 0; slot < mSlots; ++slot)
                total += mCopies[slot].getBytes();

            return total;
        }

        std::span<const Index> getOwed(std::uint32_t slot) const
        {
            assert(slot < mSlots);
            return mOwed[slot];
        }

    private:
        std::uint32_t mSlots = 1;
        std::array<BlockedBuffer, sFrameSlots> mCopies;

        /// **Runs and not a `RowDebt`, because there is no "everything" here to owe.** A row table
        /// can fill a copy whole from the rows it holds; a block table's data is the scene's, and
        /// nothing here knows how many runs there are or how to read one. A copy starts owing
        /// nothing and a load fills it through `at`.
        ///
        /// Cleared and refilled, never freed: it settles at the busiest pair of frames so far.
        std::array<std::vector<Index>, sFrameSlots> mOwed;
    };
}
