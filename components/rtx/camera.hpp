#pragma once

#include <cstdint>

#include <osg/Matrixf>
#include <osg/Vec2f>
#include <osg/Vec3f>

#include "shaders/visibility.h"

namespace Rtx
{
    /// Constants for a pinhole camera at `origin` looking `along`, which need not be a unit vector.
    ///
    /// The world's up is +Z, as Morrowind has it. A camera with no direction to look along, or one
    /// pointed straight up or straight down, has no basis; both throw `Error`. These arrive from a
    /// command line, so they are input rather than a contract, and the alternative to a message is a
    /// normalised zero vector quietly filling the image with NaN.
    Shaders::VisibilityConstants makeCameraAlong(const osg::Vec3f& origin, const osg::Vec3f& along,
        float verticalFovDegrees, std::uint32_t width, std::uint32_t height, float far);

    /// The same, from a point to look at rather than a direction.
    ///
    /// **Not what a camera far from the origin should use.** A direction recovered by subtracting
    /// two world points carries their rounding: a float ulp out where Morrowind's cells are is a
    /// hundredth of a unit, so two points a unit apart name a direction that is a fifth of a degree
    /// out and that changes every time the eye moves. Anything holding a view matrix has the
    /// direction already and should hand it over; a target is for a viewpoint written down in a
    /// file, where it is exact because it never moves.
    Shaders::VisibilityConstants makeCamera(const osg::Vec3f& origin, const osg::Vec3f& target,
        float verticalFovDegrees, std::uint32_t width, std::uint32_t height, float far);

    /// A camera from a view matrix — the world-to-eye transform anything holding a viewpoint
    /// already has, in OpenSceneGraph's convention: row vectors, and an eye space looking down its
    /// own -Z.
    ///
    /// **The basis comes out of the matrix rather than from the world's up.** `makeCameraAlong` has
    /// no roll to work from and so takes it from the world, which leaves it with no answer at all
    /// for a camera looking straight down — and straight down is what a map is. Throws `Error` for
    /// a matrix that cannot be inverted or whose basis has collapsed.
    Shaders::VisibilityConstants makeCameraFromView(const osg::Matrixf& view, float verticalFovDegrees,
        std::uint32_t width, std::uint32_t height, float near, float far);

    /// The same viewpoint with no perspective in it: every ray travels the view direction, and
    /// which one a pixel sends comes from where it sits on a box `worldWidth` by `worldHeight`
    /// centred on the eye.
    Shaders::VisibilityConstants makeOrthographicCameraFromView(const osg::Matrixf& view, float worldWidth,
        float worldHeight, std::uint32_t width, std::uint32_t height, float near, float far);

    /// Where inside its pixel frame `index` should sample, in pixels and centred on zero.
    ///
    /// **A low-discrepancy sequence and not a random one.** What an upscaler reconstructs from is
    /// the set of sub-pixel positions a few frames covered between them, so the positions have to
    /// spread evenly over the pixel rather than clump the way random draws do. Halton in bases two
    /// and three is what everything that does this uses, and its first terms are worth knowing:
    /// base two runs 1/2, 1/4, 3/4, 1/8, and base three 1/3, 2/3, 1/9.
    ///
    /// The result is in the image's axes — x right, y down — because that is what the shader adds
    /// it to.
    osg::Vec2f haltonJitter(std::uint32_t index);
}
