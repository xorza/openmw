#include "exteriorindex.hpp"

#include <components/esm3/loadcell.hpp>
#include <components/esmloader/esmdata.hpp>

namespace RtxTool
{
    ExteriorIndex::ExteriorIndex(const EsmLoader::EsmData& content)
    {
        for (const ESM::Cell& cell : content.mCells)
            if (cell.isExterior())
                mCells.emplace(std::pair(cell.getGridX(), cell.getGridY()), &cell);
    }

    const ESM::Cell* ExteriorIndex::find(int x, int y) const
    {
        const auto found = mCells.find(std::pair(x, y));
        return found == mCells.end() ? nullptr : found->second;
    }
}
