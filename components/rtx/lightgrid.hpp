#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <osg/Vec3f>
#include <osg/Vec3ui>

namespace Rtx
{
    struct Light;

    /// Which lamps can reach where, as two arrays.
    ///
    /// **A shading point should not have to ask every lamp in the cell whether it is near.** Walking
    /// them all costs the same whether one contributes or none do, and the fog made that
    /// unaffordable rather than merely wasteful: a surface asks once per hit where a march asks
    /// twenty-four times per pixel, sky included. Measured on Balmora's twenty-six lamps, the walk
    /// is 0.111 ms per lamp per frame at 1920x1080 — 2.9 ms on a frame that traced in 0.67 without
    /// it.
    ///
    /// A uniform grid, in **world space** rather than screen space, because a reflection or a bounce
    /// lands where no pixel is looking. Cell `i` owns `getIndices()[o[i] .. o[i + 1]]` with `o` the
    /// offsets: a prefix sum with a trailing sentinel, so the whole structure is two arrays however
    /// many cells it has and a lookup is two reads and no search.
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
        /// **Not the same as a grid built from no lamps**, which has one cell and two offsets in it.
        /// This has none, so `rebuild` has to run before anything looks a position up.
        LightGrid() = default;

        /// Bins `lights`, for a caller that has them at construction.
        explicit LightGrid(std::span<const Light> lights) { rebuild(lights); }

        /// Bins `lights` into the buffers this already has.
        ///
        /// **What a frame uses, and the constructor above is not.** Rebinding must not go back to
        /// the allocator: assigning a freshly built grid over this one threw away three vectors and
        /// made three more, on every frame that moved.
        void rebuild(std::span<const Light> lights);

        /// The corner cell zero starts at, and how many cells the grid is across.
        const osg::Vec3f& getOrigin() const { return mOrigin; }
        const osg::Vec3ui& getSize() const { return mSize; }

        /// One over the cell's side, which is what turns a position into a cell without a divide.
        float getInverseCell() const { return mInverseCell; }

        /// `getSize()` product plus one: every cell's start, and a sentinel so the last cell's end
        /// needs no special case.
        std::span<const std::uint32_t> getOffsets() const { return mOffsets; }

        /// Every cell's lamps, run together in cell order.
        std::span<const std::uint32_t> getIndices() const { return mIndices; }

    private:
        osg::Vec3f mOrigin;
        osg::Vec3ui mSize{ 1u, 1u, 1u };
        float mInverseCell = 1.0f;
        std::vector<std::uint32_t> mOffsets;
        std::vector<std::uint32_t> mIndices;

        /// Where each cell's next entry goes while the runs are being filled. A member for the
        /// reason the two above are refilled rather than replaced.
        std::vector<std::uint32_t> mCursor;
    };
}
