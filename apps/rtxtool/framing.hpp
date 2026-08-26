#pragma once

#include <cstdint>

#include <osg/Vec3f>

#include <components/rtx/shaders/visibility.h>

#include "lighting.hpp"
#include "placement.hpp"

namespace Rtx
{
    struct FrameExtents;
}

namespace RtxTool
{
    /// Everything one traced frame needs settled before it is traced.
    ///
    /// **One struct because the three commands that trace a frame had each grown their own block of
    /// assignments, and the blocks had drifted.** `shot` and `view` honoured `--albedo` and `bench`
    /// did not; `bench` and `view` advanced the water and `shot` did not; `shot`'s far plane had no
    /// floor under it and the other two had one at ten thousand units. Three separate blocks, and
    /// nothing in any of them said which differences were decisions and which were omissions.
    ///
    /// They are all still exactly as they were — this changes no frame. What it changes is that each
    /// disagreement is now a field somebody fills in, in one place, where the next one cannot appear
    /// without being written down.
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

        /// Far enough to cross the scene. A primary ray that reaches this has left the world.
        ///
        /// **Undecided:** `bench` and `view` hold this at no less than ten thousand units and `shot`
        /// takes eight times the scene's radius whatever that comes to, so a screenshot of a small
        /// interior sees less far than a measurement of it.
        float mFar = 0.0f;

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
