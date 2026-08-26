#include "lightgrid.hpp"

#include <algorithm>
#include <cmath>

#include "scenedesc.hpp"

namespace
{
    /// How many cells the grid may hold, and how many lamp entries across all of them.
    ///
    /// **Two budgets, and the second is not implied by the first.** A wide exterior overruns the
    /// cell count while its lamps are ordinary; one lamp with an enormous reach overruns the entry
    /// count while the grid is still small, because it lands in every cell it touches. Doubling the
    /// cell until both fit is what makes either recoverable.
    constexpr std::size_t sMaxCells = 65536;
    constexpr std::size_t sMaxEntries = 262144;

    /// The side a grid starts at, in world units — one terrain tile.
    ///
    /// A cell this size holds a lamp or two of Morrowind's, whose reaches run to a few hundred
    /// units, so it is the scale at which the list is short and the grid is small. Everything else
    /// is reached by doubling.
    constexpr float sFirstCell = 1024.0f;

    /// The cells a sphere of `reach` about `centre` touches, as a half-open box of cell coordinates.
    struct CellBox
    {
        osg::Vec3ui mLow;
        osg::Vec3ui mHigh;

        std::size_t getCount() const
        {
            return std::size_t{ mHigh.x() - mLow.x() } * (mHigh.y() - mLow.y()) * (mHigh.z() - mLow.z());
        }
    };

    /// The flat index of a cell, which is the one arithmetic the shader has to agree with.
    std::size_t cellAt(std::uint32_t x, std::uint32_t y, std::uint32_t z, const osg::Vec3ui& size)
    {
        return (std::size_t{ z } * size.y() + y) * size.x() + x;
    }

    /// Hands `visit` the flat index of every cell in `box`.
    template <typename Visit>
    void forEachCell(const CellBox& box, const osg::Vec3ui& size, Visit visit)
    {
        for (std::uint32_t z = box.mLow.z(); z < box.mHigh.z(); ++z)
            for (std::uint32_t y = box.mLow.y(); y < box.mHigh.y(); ++y)
                for (std::uint32_t x = box.mLow.x(); x < box.mHigh.x(); ++x)
                    visit(cellAt(x, y, z, size));
    }

    CellBox boxAround(
        const osg::Vec3f& centre, float reach, const osg::Vec3f& origin, float inverseCell, const osg::Vec3ui& size)
    {
        CellBox box;
        for (int axis = 0; axis < 3; ++axis)
        {
            // Clamped rather than rejected: a lamp standing outside the grid still reaches into it,
            // and a cell inside it has to know.
            const float low = (centre[axis] - reach - origin[axis]) * inverseCell;
            const float high = (centre[axis] + reach - origin[axis]) * inverseCell;

            const auto span = static_cast<float>(size[axis]);
            box.mLow[axis] = static_cast<std::uint32_t>(std::clamp(std::floor(low), 0.0f, span));
            box.mHigh[axis] = static_cast<std::uint32_t>(std::clamp(std::floor(high) + 1.0f, 0.0f, span));
        }
        return box;
    }
}

namespace Rtx
{
    void LightGrid::rebuild(std::span<const Light> lights)
    {
        osg::BoundingBoxf bounds;
        for (const Light& light : lights)
        {
            const osg::Vec3f reach(light.mReach, light.mReach, light.mReach);
            bounds.expandBy(osg::BoundingBoxf(light.mPosition - reach, light.mPosition + reach));
        }

        mOrigin = bounds.valid() ? bounds._min : osg::Vec3f();
        const osg::Vec3f extent = bounds.valid() ? bounds._max - bounds._min : osg::Vec3f();

        // The cell doubles until the grid fits both budgets. Counting the entries needs the size
        // the count is against, so each candidate is sized and then measured.
        //
        // **It ends because doubling shrinks both.** Every axis falls to a single cell once the cell
        // outgrows the extent, which is one cell holding one entry per lamp — inside both budgets
        // for any scene with fewer lamps than the entry budget allows.
        for (float cell = sFirstCell;; cell *= 2.0f)
        {
            mInverseCell = 1.0f / cell;
            for (int axis = 0; axis < 3; ++axis)
                mSize[axis] = static_cast<std::uint32_t>(std::max(std::ceil(extent[axis] / cell), 1.0f));

            const std::size_t cells = std::size_t{ mSize.x() } * mSize.y() * mSize.z();
            if (cells > sMaxCells)
                continue;

            std::size_t entries = 0;
            for (const Light& light : lights)
                entries += boxAround(light.mPosition, light.mReach, mOrigin, mInverseCell, mSize).getCount();

            if (entries <= sMaxEntries)
                break;
        }

        const std::size_t cells = std::size_t{ mSize.x() } * mSize.y() * mSize.z();

        // A counting sort: how many lamps each cell holds, then where each cell's run starts, then
        // the runs themselves. The offsets carry a trailing sentinel, so the last cell's end is read
        // the same way every other cell's is.
        mOffsets.assign(cells + 1, 0);
        for (const Light& light : lights)
            forEachCell(boxAround(light.mPosition, light.mReach, mOrigin, mInverseCell, mSize), mSize,
                [&](std::size_t cell) { ++mOffsets[cell + 1]; });

        for (std::size_t cell = 0; cell < cells; ++cell)
            mOffsets[cell + 1] += mOffsets[cell];

        mIndices.resize(mOffsets.back());
        mCursor.assign(mOffsets.begin(), mOffsets.end() - 1);
        for (std::size_t index = 0; index < lights.size(); ++index)
        {
            const Light& light = lights[index];
            forEachCell(boxAround(light.mPosition, light.mReach, mOrigin, mInverseCell, mSize), mSize,
                [&](std::size_t cell) { mIndices[mCursor[cell]++] = static_cast<std::uint32_t>(index); });
        }
    }
}
