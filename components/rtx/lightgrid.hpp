#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <osg/Vec3f>
#include <osg/Vec3ui>
#include <osg/Vec4f>

#include "runlist.hpp"

namespace Rtx
{
    struct Light;

    /// Which lamps can reach where.
    ///
    /// **A shading point should not have to ask every lamp in the cell whether it is near.** Walking
    /// them all costs the same whether one contributes or none do, and the fog made that
    /// unaffordable rather than merely wasteful: a surface asks once per hit where a march asks
    /// twenty-four times per pixel, sky included. Measured on Balmora's twenty-six lamps, the walk
    /// is 0.111 ms per lamp per frame at 1920x1080 — 2.9 ms on a frame that traced in 0.67 without
    /// it.
    ///
    /// A uniform grid, in **world space** rather than screen space, because a reflection or a bounce
    /// lands where no pixel is looking. Each cell's lamps are a run of the `RunList`, keyed by the
    /// cell's flat index.
    ///
    /// A lamp is binned into every cell its **reach** touches rather than the one cell it stands in,
    /// which is what makes the lookup complete: a cell's list is every lamp that could light it, so
    /// the shader's own distance test is a refinement and never a correction.
    ///
    /// **The grid covers what the lamps reach, and takes no bounds from anyone.** Handed the scene's
    /// instead, it would end where the geometry does — and a lamp near the edge reaches past that,
    /// so a fog step in the air above a cell would be handed an empty list and lose it. Sized to the
    /// union of the reaches, a position outside the grid is one no lamp can light, and empty is the
    /// right answer rather than a missing one.
    class LightGrid
    {
    public:
        /// An unfilled grid, for an owner that binds its lamps in a later step.
        ///
        /// **Not the same as a grid built from no lamps**, which has one cell and a two-entry list.
        /// This has none, so `rebuild` has to run before anything looks a position up.
        LightGrid() = default;

        /// Bins `lights`, for a caller that has them at construction.
        explicit LightGrid(std::span<const Light> lights) { rebuild(lights); }

        /// Bins `lights` into the list this already has.
        ///
        /// **What a frame uses, and the constructor above is not.** Rebinding must not go back to
        /// the allocator: assigning a freshly built grid over this one threw away the list's vectors
        /// and made them again, on every frame that moved.
        ///
        /// **And a world whose lamps have not moved is not binned again.** Everything this reads of
        /// a light is where it stands and how far it reaches, so the grid is a function of that
        /// sequence and of nothing else — a lamp that only flickered has the same grid, and
        /// Morrowind's lamps flicker on nearly every frame. Measured at Seyda Neen's 341 lights:
        /// 0.081 ms a frame of binning, against a compare of five and a half kilobytes.
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
        /// Whether `lights` stands exactly where the last binning's did, so the list still describes
        /// it.
        ///
        /// **Element-wise and never as a set**, which is what makes it exact: entry `i` of the list
        /// names light `i`, so what has to hold is that light `i` is still at the position and the
        /// reach it was binned at. Two lamps that swapped places are caught, and a lamp that only
        /// changed colour is not — which is right, because the grid never read its colour.
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
