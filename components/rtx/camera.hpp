#pragma once

#include <cstdint>

#include <osg/Matrixf>
#include <osg/Vec2f>
#include <osg/Vec3f>

#include "shaders/visibility.h"

namespace Rtx
{
    /// Far enough to cross any cell. One number for every camera in the fork, because it is also
    /// the sun's shadow-ray reach and what the depth buffer encodes against, so a harness that
    /// traced to a different one measured a different frame from the game.
    constexpr float sFarPlane = 200000.0f;

    /// Constants for a pinhole camera at `origin` looking `along`, which need not be a unit vector.
    /// The world's up is +Z. A zero direction, or one straight up or down, throws `Error`: these
    /// arrive from a command line, so they are input and not a contract.
    Shaders::VisibilityConstants makeCameraAlong(const osg::Vec3f& origin, const osg::Vec3f& along,
        float verticalFovDegrees, std::uint32_t width, std::uint32_t height, float far);

    /// The same, from a point to look at rather than a direction — for a viewpoint written down in
    /// a file, and not for a moving eye: a float ulp where Morrowind's cells are is a hundredth of
    /// a unit, so two points a unit apart name a direction a fifth of a degree out.
    Shaders::VisibilityConstants makeCamera(const osg::Vec3f& origin, const osg::Vec3f& target,
        float verticalFovDegrees, std::uint32_t width, std::uint32_t height, float far);

    /// A camera from a view matrix in OpenSceneGraph's convention: row vectors, and an eye space
    /// looking down its own -Z. The basis comes out of the matrix rather than from the world's up,
    /// which is what lets a map look straight down. Throws `Error` for a matrix that cannot be
    /// inverted or whose basis collapsed.
    Shaders::VisibilityConstants makeCameraFromView(const osg::Matrixf& view, float verticalFovDegrees,
        std::uint32_t width, std::uint32_t height, float near, float far);

    /// The same viewpoint with no perspective in it: every ray travels the view direction, and
    /// which one a pixel sends comes from where it sits on a box `worldWidth` by `worldHeight`
    /// centred on the eye.
    Shaders::VisibilityConstants makeOrthographicCameraFromView(const osg::Matrixf& view, float worldWidth,
        float worldHeight, std::uint32_t width, std::uint32_t height, float near, float far);

    /// Where inside its pixel frame `index` should sample, in pixels and centred on zero, in the
    /// image's axes. Halton in bases two and three — 1/2, 1/4, 3/4, 1/8 and 1/3, 2/3, 1/9 —
    /// because an upscaler reconstructs from the positions a few frames covered between them, and
    /// random draws clump.
    osg::Vec2f haltonJitter(std::uint32_t index);
}
