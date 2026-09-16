#pragma once

#include <span>

#include <osg/Vec3f>
#include <osg/Vec4f>

namespace Rtx
{
    /// One vertex of a debug line or triangle: where, in world units, and what colour — with its
    /// alpha, display-referred, as the game's own debug drawers paint them.
    struct DebugVertex
    {
        osg::Vec3f mPosition;
        osg::Vec4f mColour;
    };

    /// What the game's debug modes drew this frame — the navmesh, the pathgrid, the actors' paths,
    /// the recast mesh, the collision shapes — as the lines and triangles a ray cannot meet, for a
    /// pass to draw over the picture. Spans the caller's own lists for the length of the call, and
    /// empty on every frame no mode is on, which is nearly every frame.
    struct DebugLines
    {
        std::span<const DebugVertex> mLines;
        std::span<const DebugVertex> mTriangles;

        bool empty() const { return mLines.empty() && mTriangles.empty(); }
    };
}
