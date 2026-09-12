#include "lightgrid.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "scenedesc.hpp"

namespace
{
    /// How many cells the grid may hold, and how many lamp entries across all of them. Two
    /// budgets, because a wide exterior overruns the first and one lamp with an enormous reach
    /// overruns the second, and doubling the cell until both fit recovers either.
    constexpr std::size_t sMaxCells = 65536;
    constexpr std::size_t sMaxEntries = 262144;

    /// The side a grid starts at, in world units — a quarter of a terrain tile, about half a
    /// lamp's reach, because a cell of side `c` lists every lamp within `reach + c` of it. At one
    /// tile a point in the Guild of Mages weighed twenty-one lamps for the one or two that reached
    /// it, and at a quarter tile it weighs twelve; at an eighth the entries grow eightfold for one
    /// lamp fewer. An exterior overruns the cell budget at this size and doubles back to a tile.
    constexpr float sFirstCell = 256.0f;

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
    bool LightGrid::standsWhereItWas(std::span<const Light> lights) const
    {
        // A list nothing has made yet says nothing about where the lamps are, and a world of no
        // lamps cannot be told from one by the lengths alone — both are nought. `RunList::start` is
        // what puts the first entry in, so an empty list is a grid that was never built.
        if (mList.getWhole().empty() || mBinnedOn.size() != lights.size())
            return false;

        for (std::size_t at = 0; at < lights.size(); ++at)
        {
            const Light& light = lights[at];
            if (mBinnedOn[at] != osg::Vec4f(light.mPosition, light.mReach))
                return false;
        }

        return true;
    }

    void LightGrid::rebuild(std::span<const Light> lights)
    {
        if (standsWhereItWas(lights))
            return;

        mBinnedOn.clear();
        mBinnedOn.reserve(lights.size());
        for (const Light& light : lights)
            mBinnedOn.emplace_back(light.mPosition, light.mReach);

        osg::BoundingBoxf bounds;
        for (const Light& light : lights)
        {
            const osg::Vec3f reach(light.mReach, light.mReach, light.mReach);
            bounds.expandBy(osg::BoundingBoxf(light.mPosition - reach, light.mPosition + reach));
        }

        mOrigin = bounds.valid() ? bounds._min : osg::Vec3f();
        const osg::Vec3f extent = bounds.valid() ? bounds._max - bounds._min : osg::Vec3f();

        // The cell doubles until the grid fits both budgets. It ends because every axis falls to
        // a single cell once the cell outgrows the extent, which is one entry per lamp.
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

        mList.start(cells);
        for (const Light& light : lights)
            forEachCell(boxAround(light.mPosition, light.mReach, mOrigin, mInverseCell, mSize), mSize,
                [&](std::size_t cell) { mList.count(cell); });

        mList.place();
        for (std::size_t index = 0; index < lights.size(); ++index)
        {
            const Light& light = lights[index];
            forEachCell(boxAround(light.mPosition, light.mReach, mOrigin, mInverseCell, mSize), mSize,
                [&](std::size_t cell) { mList.put(cell, static_cast<std::uint32_t>(index)); });
        }
    }
}
