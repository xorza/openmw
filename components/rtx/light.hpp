#pragma once

#include <osg/Vec3f>

namespace Rtx
{
    /// One point light, placed in the world.
    ///
    /// Everything here is derived rather than read. A `LIGH` record carries a colour and a radius
    /// and **no intensity at all** — the original renderer had a fixed attenuation curve and no
    /// physical units, so brightness fell out of the curve and there is no authored value to be
    /// faithful to.
    ///
    /// **Its own header because what makes a light is not what holds one.** `makeLight` turns a
    /// `LIGH` record into one of these and needs nothing else of the scene. Kept in `scenedesc.hpp`,
    /// it cost every reader of five fields the whole scene description — its tables, its
    /// allocators and two shader headers. `sun.hpp` is here for the same reason.
    struct Light
    {
        osg::Vec3f mPosition;

        /// Radiant intensity, linear, with the colour folded in.
        ///
        /// Scaled by the square of the recorded radius, which is what makes a lantern and a candle
        /// differ by their size rather than by an arbitrary per-light number.
        osg::Vec3f mIntensity;

        /// How far the light reaches, beyond which it contributes exactly nothing.
        ///
        /// **Not the recorded radius.** Morrowind's radii run 64 to 256 units in an interior — a
        /// metre to three and a half — because a fixed falloff curve lit the lamp's own post and an
        /// ambient term filled the room. Here the ambient is real light and the lamps have to be
        /// what lights the place, so the reach is stretched while the brightness is not.
        float mReach = 0.0f;

        /// How big the glowing part is, in world units.
        ///
        /// **Not the recorded radius**, and for once not a stretched version of it: this is the
        /// flame rather than the room it lights. `makeLight` is the one place it is derived and
        /// says what from; zero, which is what a light built by hand carries, is a point.
        ///
        /// A shadow ray opens to it, so a lamp with one casts a penumbra as wide as it is. It is
        /// also what stops the falloff running away at the lamp itself, which is where the air
        /// beside one is sampled — `falloff` says what that drew before.
        float mSourceRadius = 0.0f;

        /// How far short of the centre that ray stops.
        ///
        /// **A clearance, and it is not the same question as the size.** A lamp sits inside its own
        /// fitting — a lantern's frame, a sconce's bracket, a candle's holder — so a ray that runs
        /// all the way to the light ends among that fitting and comes back as fully shadowed. What
        /// `makeLight` estimates the fitting to be is the wider of the two.
        float mClearance = 0.0f;
    };
}
