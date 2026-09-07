#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "index.hpp"

namespace Rtx
{
    /// **The lowest free slot, and never the last one freed.** A slot is one row of a table and
    /// every row is the same size, so any of them would hold the row — but which one it is decides
    /// what a run draws, because a slot is what a material points at and what a structure is built
    /// in. `Rtx::Identity` hashes by address, so a sweep retires in whatever order the allocator
    /// left its map in, and a list taken from the back then hands the same live set different slots
    /// in two processes. Taking the lowest makes the answer a function of what is standing rather
    /// than of the order the dead left in. Measured on `one-cell-walk`: the `materials` and
    /// `textures` columns differed from frame 2 on 89 frames of 90.
    ///
    /// Every table takes its slots through this pair, `PlacementTable` included.
    inline Index takeFreeSlot(std::vector<Index>& free)
    {
        assert(!free.empty() && "a slot taken from a list with none on it");

        std::pop_heap(free.begin(), free.end(), std::greater<>());
        const Index index = free.back();
        free.pop_back();

        return index;
    }

    /// Puts `index` back, so the next `takeFreeSlot` may answer with it.
    inline void freeSlot(std::vector<Index>& free, const Index index)
    {
        free.push_back(index);
        std::push_heap(free.begin(), free.end(), std::greater<>());
    }

    /// Puts `row` in a slot of `table` nothing stands in, or on the end where there is none.
    template <class Row>
    Index takeSlot(std::vector<Row>& table, std::vector<Index>& free, const Row& row)
    {
        if (free.empty())
        {
            table.push_back(row);
            return static_cast<Index>(table.size() - 1);
        }

        const Index index = takeFreeSlot(free);
        table[index] = row;

        return index;
    }

    /// A byte per row of a table, set for everything a sweep must not free: what `keep` names, and
    /// every slot already on `free`.
    ///
    /// @return how many distinct rows `keep` named, which the free slots are not among.
    ///
    /// **Distinct, which is what lets duplicates and any order be fine.** A caller compares this
    /// against how many rows are live to decide whether anything died, so a span measured by its
    /// length would read as a larger set than it is the moment a caller named one row twice.
    inline std::size_t markKept(
        std::vector<std::uint8_t>& flags, std::size_t count, std::span<const Index> keep, std::span<const Index> free)
    {
        // Cleared before it is grown, so the fill reaches every row rather than only the rows past
        // the length the last sweep left.
        flags.clear();
        flags.resize(count, 0);

        std::size_t distinct = 0;
        for (const Index index : keep)
        {
            assert(index < count);
            distinct += flags[index] == 0 ? 1 : 0;
            flags[index] = 1;
        }

        for (const Index index : free)
            flags[index] = 1;

        return distinct;
    }
}
