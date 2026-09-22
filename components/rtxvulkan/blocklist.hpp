#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

#include <components/rtx/result.hpp>
#include <components/rtx/runs.hpp>

namespace Rtx
{
    /// Where a run was put: which block, and the run inside it.
    struct BlockRun
    {
        std::uint32_t mBlock = 0;
        Run mRun;

        bool empty() const { return mRun.empty(); }
    };

    /// A list of blocks a `RunAllocator` runs over each, the shape `StructureStorage` has: a
    /// block is made once and never moved, a run is taken out of the
    /// first block that fits it, a run given back is merged with what it touches, and a block that
    /// empties goes back — except the last of its kind, or a pool that emptied and refilled would
    /// free and make a block on alternate frames. A slot is never removed, because a run names its
    /// block by index; a block that went back leaves its slot for the next one made.
    ///
    /// @tparam Block holds `RunAllocator mRuns`, `std::uint32_t mCapacity` in the same unit the
    ///         runs are counted in — nought for a slot whose block went back — and `void retire()`,
    ///         which hands the block's own resource back and leaves the capacity at nought.
    template <class Block>
    class BlockList
    {
    public:
        /// `units` of room, out of the first live block `fits` admits that has them, or out of a
        /// block `make(units)` makes into the first retired slot or at the end — or why there is
        /// none, where `make` could not make one. Asked for and given back rather than measured
        /// first: where a run goes is best fit over a free list, and asking whether one would fit
        /// is that rule written a second time.
        ///
        /// @param make `Result<Block, std::string_view>(std::uint32_t least, std::uint32_t slot)`:
        ///        a block holding at least `least` units, its `mCapacity` set, its `mRuns`
        ///        untouched, for the slot it will take — which a name can carry — or why the device
        ///        has none. Built whole before it joins the list, so a make that fails leaves the
        ///        list holding what it held.
        template <class Fits, class Make>
        Result<BlockRun, std::string_view> take(const std::uint32_t units, Fits&& fits, Make&& make)
        {
            std::size_t retired = mBlocks.size();

            for (std::size_t at = 0; at < mBlocks.size(); ++at)
            {
                Block& block = mBlocks[at];
                if (block.mCapacity == 0)
                {
                    retired = std::min(retired, at);
                    continue;
                }

                if (!fits(block))
                    continue;

                const Run run = block.mRuns.allocate(units);
                if (block.mRuns.getEnd() <= block.mCapacity)
                    return BlockRun{ static_cast<std::uint32_t>(at), run };

                block.mRuns.release(run);
            }

            const bool append = retired == mBlocks.size();
            const auto at = static_cast<std::uint32_t>(retired);

            Result<Block, std::string_view> made = make(units, at);
            if (!made.isOk())
                return Err{ made.error() };

            if (append)
                mBlocks.push_back(std::move(made.value()));
            else
                mBlocks[at] = std::move(made.value());

            return BlockRun{ at, mBlocks[at].mRuns.allocate(units) };
        }

        /// Gives a run back, and retires the block it emptied where `mayRetire(block)` says the
        /// block is not the last of its kind.
        template <class MayRetire>
        void give(const BlockRun& placed, MayRetire&& mayRetire)
        {
            Block& block = mBlocks[placed.mBlock];
            block.mRuns.release(placed.mRun);

            if (block.mRuns.getEnd() == 0 && mayRetire(block))
                block.retire();
        }

        Block& at(const std::size_t index) { return mBlocks[index]; }
        const Block& at(const std::size_t index) const { return mBlocks[index]; }

        auto begin() { return mBlocks.begin(); }
        auto end() { return mBlocks.end(); }
        auto begin() const { return mBlocks.begin(); }
        auto end() const { return mBlocks.end(); }

    private:
        std::vector<Block> mBlocks;
    };
}
