#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "index.hpp"

namespace Rtx
{
    /// Puts `row` in a slot of `table` nothing stands in, or on the end where there is none, and
    /// says which.
    ///
    /// **Any free slot will do.** A slot is one row of a table and every row is the same size; what
    /// varies in length — the geometry, the layers, the runs — the allocators have already placed.
    /// Taken from the back, because there is no fit to find.
    template <class Row>
    Index takeSlot(std::vector<Row>& table, std::vector<Index>& free, const Row& row)
    {
        if (!free.empty())
        {
            const Index index = free.back();
            free.pop_back();
            table[index] = row;
            return index;
        }

        table.push_back(row);
        return static_cast<Index>(table.size() - 1);
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
