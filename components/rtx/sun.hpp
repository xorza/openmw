#pragma once

#include <osg/Vec3f>

namespace Rtx
{
    /// The sun, as a directional light and as something to look at.
    ///
    /// Far enough away that its rays are parallel, so it has a direction and no position, and its
    /// shadow ray runs to the end of the world rather than to an emitter.
    ///
    /// **One sun, where the game keeps five dials for one**, and the one thing to know here is that
    /// nothing may fill these fields itself: `makeSkylight` builds every one of them for the world
    /// and its header says what goes wrong when they are set apart. The one other builder is
    /// `OffscreenTrace::setLight`, for a picture inside the interface lit by a flat light with no
    /// hour behind it — where the irradiance is the light's and nought where the light is.
    ///
    /// **Its own header because a scene has no sun in it.** `SceneDesc` holds no such member: a sun
    /// is what the frame's *world* is doing, which is `Skylight` and `describeWorld`. Kept in
    /// `scenedesc.hpp`, it cost every reader of three vectors the whole scene description — its
    /// tables, its allocators and two shader headers.
    struct Sun
    {
        /// Where the sun stands, unit — and so `-mPosition` is where its light travels.
        /// `Sky::SunPlacement::mPosition` is what this is read from and says why it is kept through
        /// a night that has no sun in it.
        osg::Vec3f mPosition{ 0.0f, 0.0f, 1.0f };

        /// Irradiance on a surface square to it, linear.
        ///
        /// **Zero exactly when there is no sun**, which is the invariant the whole type exists for,
        /// and `Sky::SunPlacement::mShare` is where that invariant is argued: this is the same
        /// question one layer down, so what is nothing there is nothing here. Everything the sun
        /// does is gated on this one test, so a sun cannot shadow without being drawn or be drawn
        /// without lighting.
        osg::Vec3f mIrradiance;

        /// What the disc is painted with, linear — **not the hue of `mIrradiance`.** The colour a
        /// weather gives its sunlight is the sky's as much as the sun's, which is why it is blue at
        /// night; the disc has its own, and `Sky::sunDiscAt` is where it comes from. The weather's
        /// own glare is folded in, so an overcast sun is a paler one.
        osg::Vec3f mDiscColour{ 1.0f, 1.0f, 1.0f };
    };
}
