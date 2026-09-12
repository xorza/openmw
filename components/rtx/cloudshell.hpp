#pragma once

#include <osg/Vec2f>
#include <osg/Vec3f>

#include <components/vfs/pathutil.hpp>

namespace osg
{
    class Node;
}

namespace Resource
{
    class SceneManager;
}

namespace Rtx
{
    /// The surface Morrowind's cloud deck is painted on, read off the mesh the rasterizer draws: a
    /// cap of radius 1000 falling from 292 above the eye to 85 at its rim. A ray tracer wants the
    /// shape and not the cap, because a plane and a cap disagree about where a direction lands —
    /// at fifteen degrees the cap's tiles are twice a plane's — and the cap is why a Morrowind sky
    /// reads as a few great clouds. It is a layer over a round world seen from underneath,
    /// `z = h - k r²` with `k = 1 / 2R`, so the two numbers are a height and a curvature. Read
    /// rather than transcribed, because the mesh is content, and as ratios, so the scale cancels.
    struct CloudShell
    {
        /// How high the layer hangs over the eye, measured in texture tiles, along each of the
        /// sheet's two axes. Signed, and Morrowind's `v` is the negative one, or a sheet laid the
        /// other way up drifts its clouds *into* the storm.
        osg::Vec2f mTiles;

        /// How far the layer falls away from the eye, as `k · h` — the sagitta's curvature times the
        /// height, which is what makes it a pure number. Nought is a flat plane, which is what the
        /// deck was drawn on before this was read.
        float mCurvature = 0.0f;

        /// Where the engine's own fade sits, as three crossing radii in texture tiles: the last one
        /// carrying a whole deck, the one carrying a quarter of it, and the rim past which there is
        /// none — `ModVertexAlphaVisitor::Clouds` by way of the mesh, whose fade is linear in
        /// radius between rings. On Morrowind's own cap 1.17, 1.72 and 2.50 tiles.
        osg::Vec3f mRings;
    };

    /// Reads it off the cloud mesh the configuration names, which the host passes in.
    ///
    /// A missing or unreadable mesh comes back with nothing in it, which draws no deck rather than
    /// failing — the same answer `readNightSky` gives, and for the same reason.
    CloudShell readCloudShell(Resource::SceneManager& scenes, VFS::Path::NormalizedView mesh);

    /// The same reading, of a mesh already in hand.
    CloudShell readCloudShell(osg::Node& mesh);
}
