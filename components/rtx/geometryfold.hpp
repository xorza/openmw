#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <osg/Vec3f>

#include "shapefold.hpp"

namespace osg
{
    class Geometry;
}

namespace Rtx
{
    /// Reads one geometry's triangles and folds away the reversed twin the content drew for each
    /// sheet's back.
    ///
    /// **What a mesh's arrival costs, in one place.** The two steps are one operation — a strip is
    /// not a mesh until it is triangles, and a doubled card is not a mesh until it is one copy — so
    /// a caller that spelled them out itself would own the collector, the fold and the buffer
    /// between them. A second caller that owned them again would be a second answer about what a
    /// shape is.
    ///
    /// **An object because the operation keeps its tables.** `ShapeFold` grows its scratch to the
    /// largest mesh it has folded and the triangles below do the same, so a run of arrivals stops
    /// resizing; a fold made per call would go to the allocator on every mesh of a cell.
    ///
    /// **Not thread-safe, and one instance a thread.** Everything it keeps is scratch it overwrites,
    /// so two threads folding through one of these would read each other's triangles.
    class GeometryFold
    {
    public:
        /// Collects `geometry`'s triangles, folds them, and says what the shape came to.
        ///
        /// @param positions what the fold matches its twins on. **The caller's and not the
        ///        geometry's own array**, because a morphed face is folded against its base target
        ///        and the geometry holds the source's.
        /// @return false where the geometry holds no triangle to read, which is a drawable that
        ///         mirrors nothing.
        bool read(const osg::Geometry& geometry, std::span<const osg::Vec3f> positions, FoldedShape& shape);

        /// The triangles the fold kept, valid until the next `read`.
        std::span<const std::uint32_t> getIndices() const { return mIndices; }

    private:
        ShapeFold mFold;

        // Refilled per drawable rather than reallocated, because a cell is tens of thousands of
        // them.
        std::vector<std::uint32_t> mIndices;
    };
}
