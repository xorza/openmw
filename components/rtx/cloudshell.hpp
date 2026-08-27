#pragma once

#include <osg/Vec2f>

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
    /// The surface Morrowind's cloud deck is painted on, read off the mesh the rasterizer draws.
    ///
    /// **A layer over a curved world rather than a plane over a flat one.** `sky_clouds_01.nif` is a
    /// cap of radius 1000 that falls from 292 above the eye at its middle to 85 at its rim, and its
    /// texture is a plain projection of the ground plane at 400 units to a tile. A ray tracer does
    /// not want the cap — it is a thing to rasterize — but it does want the shape, because a plane
    /// and a cap disagree about where a direction lands and the disagreement grows all the way down:
    /// at forty-five degrees the cap gives tiles a third larger than a plane at the same height, at
    /// fifteen degrees twice as large, and toward the horizon the plane's run away without bound
    /// while the cap's arrive at a rim. The cap is why a Morrowind sky reads as a few great clouds
    /// and a plane is why one drawn on a plane reads as wallpaper.
    ///
    /// A cap of that shape is what a flat layer over a round world looks like from underneath —
    /// `z = h - k r²` is the sagitta with `k = 1 / 2R` — so the two numbers here are a height and a
    /// curvature, and the rim is the horizon that curvature puts the layer's edge at.
    ///
    /// **Read rather than transcribed**, for the reason `NightSky` gives: the mesh is content and
    /// content is what a mod replaces. Both numbers are ratios, so the scale the mesh happens to be
    /// modelled at cancels and nothing here depends on 1000 being 1000.
    struct CloudShell
    {
        /// How high the layer hangs over the eye, measured in texture tiles, along each of the
        /// sheet's two axes.
        ///
        /// **Signed, and Morrowind's `v` is the negative one.** The mesh runs `u` with the world's
        /// `x` and `v` against its `y`, and the scroll that carries the deck along is added to `v` —
        /// so a sheet laid the other way up drifts its clouds *into* the storm instead of along with
        /// it. The magnitude is the same on both axes for any sheet laid square, which every one
        /// this renderer has seen is.
        osg::Vec2f mTiles;

        /// How far the layer falls away from the eye, as `k · h` — the sagitta's curvature times the
        /// height, which is what makes it a pure number. Nought is a flat plane, which is what the
        /// deck was drawn on before this was read.
        float mCurvature = 0.0f;
    };

    /// Reads it off `Models/skyclouds`.
    ///
    /// A missing or unreadable mesh comes back with nothing in it, which draws no deck rather than
    /// failing — the same answer `readNightSky` gives, and for the same reason.
    CloudShell readCloudShell(Resource::SceneManager& scenes);

    /// The same reading, of a mesh already in hand.
    CloudShell readCloudShell(osg::Node& mesh);
}
