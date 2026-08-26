#include "cellgrid.hpp"

#include <algorithm>
#include <utility>

namespace Misc
{
    void CellGrid::listCells(std::vector<osg::Vec2i>& cells) const
    {
        const auto side = static_cast<std::size_t>(2 * mHalfSize + 1);
        cells.clear();
        cells.reserve(side * side);

        for (int x = mCentre.x() - mHalfSize; x <= mCentre.x() + mHalfSize; ++x)
            for (int y = mCentre.y() - mHalfSize; y <= mCentre.y() + mHalfSize; ++y)
                cells.emplace_back(x, y);

        const auto priority = [this](const osg::Vec2i& cell) {
            return std::pair{ std::abs(cell.x() - mCentre.x()) + std::abs(cell.y() - mCentre.y()),
                std::abs(cell.x()) + std::abs(cell.y()) };
        };

        std::sort(cells.begin(), cells.end(),
            [&](const osg::Vec2i& left, const osg::Vec2i& right) { return priority(left) < priority(right); });
    }
}
