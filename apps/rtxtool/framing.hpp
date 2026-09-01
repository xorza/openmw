#pragma once

#include <cstdint>

#include <osg/Vec3f>

#include <components/rtx/camera.hpp>
#include <components/rtx/shaders/visibility.h>

#include "lighting.hpp"
#include "placement.hpp"

namespace Rtx
{
    struct FrameExtents;
}

namespace RtxTool
{
    /// What a measured run advances the world by per frame, which is `PosedActors`' own step and
    /// not a second opinion about it: an actor has to move the same amount per frame here as it
    /// does in the game, and two clocks that disagreed would make a run irreproducible in the one
    /// way that matters.
    constexpr float sStepRate = 60.0f;

    /// How long one of those frames stands for, which is what a measured frame tells the renderer.
    ///
    /// **The eye's adaptation and the upscaler's tuning are the last things in a frame that would
    /// otherwise run on the wall.** A run that steps everything else by the frame index and lets
    /// those read the clock draws a different picture every time it is run, which is a run that
    /// cannot be compared with itself. `Rtx::FrameOptions::mSinceLast` is where this goes.
    constexpr float sStepSeconds = 1.0f / sStepRate;

    /// Everything one traced frame needs settled before it is traced.
    ///
    /// **One struct because the three commands that trace a frame had each grown their own block of
    /// assignments, and the blocks had drifted.** `shot` and `view` honoured `--albedo` and `bench`
    /// did not; `bench` and `view` advanced the water and `shot` did not; each of the three carried
    /// a far plane of its own. Three separate blocks, and nothing in any of them said which
    /// differences were decisions and which were omissions.
    ///
    /// What is left of them is a field somebody fills in, in one place, where the next disagreement
    /// cannot appear without being written down. The far plane is settled here rather than by a
    /// caller — `mFar` says why.
    struct Framing
    {
        osg::Vec3f mOrigin;

        /// Which way it faces, rather than a point it faces.
        ///
        /// **A direction and not a target**, because a target is where a rounding error lives: two
        /// world points out where Morrowind's cells are name a direction only to about a fifth of a
        /// degree, and it lands somewhere else every time the eye moves. A caller holding a view
        /// direction hands it over; one holding two points written down in a file uses `lookingFrom`.
        osg::Vec3f mForward;

        float mFieldOfView = 90.0f;

        /// Far enough to cross the world, which is the game's own plane and not a fit to the scene.
        ///
        /// **Every command traces to the same distance now, because the distance is not only a
        /// cost.** It is the sun's shadow-ray reach, the unbounded water path and the depth encode,
        /// so a `shot` fitted to eight scene radii and a `bench` floored at ten thousand units
        /// answered different questions about the same place. `Rtx::sFarPlane` says the rest.
        float mFar = Rtx::sFarPlane;

        CellLighting mLighting;

        /// How much of the light baked into a vanilla texture is taken back out.
        float mDelight = 1.0f;

        /// Draw the albedo the materials recovered instead of tracing the frame.
        ///
        /// **Undecided:** `shot` and `view` take this from `--albedo`; `bench` has no such option and
        /// leaves it off.
        bool mShowAlbedo = false;

        /// What the bounce's sampler and the upscaler's jitter are walked by.
        ///
        /// Held at zero for a frame that has to come out the same twice, which is what a screenshot
        /// and a pixel test want; anything drawing a sequence passes its frame index.
        std::uint32_t mFrame = 0;

        /// Framing for a camera written down as two points, which is what `views.cfg` holds.
        ///
        /// Exact where the viewpoint never moves, which is the case a file is for. A flying camera
        /// sets `mForward` itself, for the reason that field gives.
        static Framing lookingFrom(const Placement& placement);
    };

    /// The constants the renderer takes, for a frame traced at `extents`.
    ///
    /// Throws `Rtx::Error` where the direction cannot be built from — nothing to look along, or
    /// straight up, which has no roll. Both come off a command line or a view file, so they are
    /// input rather than a broken contract.
    Rtx::Shaders::VisibilityConstants makeFrameConstants(const Framing& framing, const Rtx::FrameExtents& extents);
}
