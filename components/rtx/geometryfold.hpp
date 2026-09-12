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
    /// sheet's back — one operation, so there is one answer about what a shape is. An object
    /// because it keeps its scratch at the largest mesh it folded, so a run of arrivals stops
    /// resizing. Not thread-safe, and one instance a thread.
    class GeometryFold
    {
    public:
        /// Collects `geometry`'s triangles, folds them, and says what the shape came to.
        ///
        /// @param positions what the fold matches its twins on — the caller's array, because a
        ///        morphed face is folded against its base target and the geometry holds the source.
        /// @return false where the geometry holds no triangle to read.
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
