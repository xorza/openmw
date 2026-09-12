#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <osg/Vec3f>
#include <osg/Vec3ui>
#include <osg/Vec4f>

#include "runs.hpp"

namespace Rtx
{
    struct Light;

    /// Which lamps can reach where: a uniform grid in world space, because a bounce lands where no
    /// pixel is looking, and a fog march asking every lamp twenty-four times a pixel was several
    /// times the trace. A lamp is binned into every cell its reach touches, so the shader's own
    /// distance test is a refinement and never a correction. The grid covers what the lamps reach
    /// and takes no bounds from the scene, so a fog step in the air above a cell is not handed an
    /// empty list.
    class LightGrid
    {
    public:
        /// An unfilled grid, for an owner that binds its lamps in a later step. Not the same as a
        /// grid built from no lamps, which has one cell and a two-entry list: this has none, so
        /// `rebuild` has to run before anything looks a position up.
        LightGrid() = default;

        /// Bins `lights`, for a caller that has them at construction.
        explicit LightGrid(std::span<const Light> lights) { rebuild(lights); }

        /// Bins `lights` into the list this already has, without going back to the allocator, and
        /// not at all where the lamps have not moved: a lamp that only flickered has the same grid,
        /// and Morrowind's lamps flicker on nearly every frame.
        void rebuild(std::span<const Light> lights);

        /// The corner cell zero starts at, and how many cells the grid is across.
        const osg::Vec3f& getOrigin() const { return mOrigin; }
        const osg::Vec3ui& getSize() const { return mSize; }

        /// One over the cell's side, which is what turns a position into a cell without a divide.
        float getInverseCell() const { return mInverseCell; }

        /// Every cell's lamps, keyed by `(z * size.y + y) * size.x + x`, in the order the lamps were
        /// given.
        const RunList& getList() const { return mList; }

    private:
        /// Whether `lights` stands exactly where the last binning's did, element-wise, because
        /// entry `i` of the list names light `i`. A lamp that only changed colour is not caught,
        /// because the grid never read its colour.
        bool standsWhereItWas(std::span<const Light> lights) const;

        osg::Vec3f mOrigin;
        osg::Vec3ui mSize{ 1u, 1u, 1u };
        float mInverseCell = 1.0f;
        RunList mList;

        /// Where each light stood when the list was last made, and how far it reached — `xyz` and
        /// `w`. Refilled beside the list and never freed.
        std::vector<osg::Vec4f> mBinnedOn;
    };
}
