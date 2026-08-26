#ifndef OPENMW_COMPONENTS_MISC_CELLGRID_H
#define OPENMW_COMPONENTS_MISC_CELLGRID_H

#include <cstdlib>
#include <vector>

#include <osg/Vec2i>
#include <osg/Vec4i>

#include <components/misc/constants.hpp>

namespace Misc
{
    /// The square of exterior cells a host keeps active, and the one rule every answer about it
    /// comes from.
    ///
    /// **A centre and a half size, and nothing else stored.** A host asks a cell grid two questions
    /// — which cells it holds, and what rectangle the terrain is told — and they have to describe
    /// the same square. Derived apart they drift, and a cell the two disagree about is in neither
    /// picture: its own references unloaded, and `Terrain::ObjectPaging` still refusing to page what
    /// it believes is the active grid. That reads as a corridor of ground with the trees taken off
    /// it, and it is what a second derivation of this square cost once already.
    class CellGrid
    {
    public:
        CellGrid() = default;

        CellGrid(const osg::Vec2i& centre, int halfSize)
            : mCentre(centre)
            , mHalfSize(halfSize)
        {
        }

        const osg::Vec2i& getCentre() const { return mCentre; }
        int getHalfSize() const { return mHalfSize; }

        /// **Minimum inclusive and maximum exclusive**, which is how `Terrain::World::setActiveGrid`
        /// and the quad tree's own bounds test read it.
        osg::Vec4i getBounds() const
        {
            return osg::Vec4i(mCentre.x() - mHalfSize, mCentre.y() - mHalfSize, mCentre.x() + mHalfSize + 1,
                mCentre.y() + mHalfSize + 1);
        }

        bool contains(int x, int y) const
        {
            return std::abs(x - mCentre.x()) <= mHalfSize && std::abs(y - mCentre.y()) <= mHalfSize;
        }

        /// Every cell of the square, **nearest to the centre first and ties broken by distance to
        /// the origin**.
        ///
        /// That order is the load order, and it is not cosmetic: a run timing a camera across a cell
        /// boundary measures which cell arrives first, so a scanline fill is a different benchmark.
        ///
        /// Fills `cells` rather than returning one, so a caller may keep its buffer.
        void listCells(std::vector<osg::Vec2i>& cells) const;

    private:
        osg::Vec2i mCentre;
        int mHalfSize = Constants::CellGridRadius;
    };
}

#endif
