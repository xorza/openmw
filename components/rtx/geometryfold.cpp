#include "geometryfold.hpp"

#include <osg/Geometry>
#include <osg/TriangleIndexFunctor>

namespace Rtx
{
    namespace
    {
        /// Collects triangle indices whatever primitive mode the geometry used.
        ///
        /// Strips, fans and quads all arrive here as triangles, which is the only form an
        /// acceleration structure takes. Degenerate triangles — how a strip restarts — are dropped:
        /// they contribute no surface and a zero-area triangle in a BLAS is wasted traversal.
        struct TriangleCollector
        {
            std::vector<std::uint32_t>* mIndices = nullptr;

            void operator()(unsigned int a, unsigned int b, unsigned int c) const
            {
                if (a == b || b == c || a == c)
                    return;

                mIndices->push_back(a);
                mIndices->push_back(b);
                mIndices->push_back(c);
            }
        };
    }

    bool GeometryFold::read(
        const osg::Geometry& geometry, const std::span<const osg::Vec3f> positions, FoldedShape& shape)
    {
        mIndices.clear();

        osg::TriangleIndexFunctor<TriangleCollector> collector;
        collector.mIndices = &mIndices;
        geometry.accept(collector);

        if (mIndices.empty())
            return false;

        shape = mFold.fold(positions, mIndices);

        return true;
    }
}
