#pragma once

#include <cstdint>
#include <span>

#include <osg/Vec2f>
#include <osg/Vec3f>

namespace Rtx
{
    /// One mesh's vertices and its triangles, as everything that hands a mesh over states them.
    ///
    /// **Named fields and not four spans in a row.** Every attribute but the positions may be
    /// empty, so a caller that brings only positions and indices wrote `{}, {}` in the middle of an
    /// argument list and a fifth attribute moved every one of them. What each span is is now said
    /// at the call.
    ///
    /// The attribute arrays are parallel: each is either empty or exactly as long as `mPositions`,
    /// and one vertex id indexes all of them. `mIndices` is mesh-local and a whole number of
    /// triangles.
    struct MeshArrays
    {
        std::span<const osg::Vec3f> mPositions;

        /// Empty where the geometry names no normal, which is read as "use the triangle's plane".
        std::span<const osg::Vec3f> mNormals;

        std::span<const osg::Vec2f> mTexCoords;

        /// The per-vertex colour, in linear light. Empty is white, which is what the multiply a
        /// hit makes of it needs it to be.
        ///
        /// **Decoded where it is read and not where it is used.** Morrowind stores these
        /// display-encoded, a blend of stored bytes is not the encoding of the blend, and a hit
        /// interpolates across a triangle — so the decode belongs on the side that runs once per
        /// vertex. `Rtx::decodeColour` is the call, and `Rtx::toLinear` says why the order matters.
        std::span<const osg::Vec3f> mColours;

        std::span<const std::uint32_t> mIndices;
    };
}
