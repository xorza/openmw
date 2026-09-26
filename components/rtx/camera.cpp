#include "camera.hpp"

#include <cassert>
#include <cmath>
#include <limits>
#include <optional>

#include <osg/Math>
#include <osg/Matrixd>

#include "contract.hpp"
#include "radicalinverse.hpp"
#include "shaders/camera.h"
#include "shaders/scene.h"

namespace Rtx
{
    namespace
    {
        /// The half-extents of the image plane at one unit ahead, and the angle one pixel covers.
        struct Spread
        {
            float mHalfWidth = 0.0f;
            float mHalfHeight = 0.0f;

            /// The vertical angle one pixel covers. Pixels are square here, so one number does for
            /// both.
            float mAngle = 0.0f;
        };

        Spread spreadOf(float verticalFovDegrees, std::uint32_t width, std::uint32_t height)
        {
            const float halfHeight = std::tan(osg::DegreesToRadians(verticalFovDegrees) * 0.5f);

            return Spread{
                .mHalfWidth = halfHeight * static_cast<float>(width) / static_cast<float>(height),
                .mHalfHeight = halfHeight,
                .mAngle = std::atan(2.0f * halfHeight / static_cast<float>(height)),
            };
        }

        /// A viewpoint before anything has described the world over it, one statement for the
        /// three builders. `Shaders::VisibilityConstants` is a header `glslc` reads as well, so it
        /// can hold no default member initialisers of its own.
        Shaders::VisibilityConstants beforeWorld(const osg::Vec3f& origin, float near, float far)
        {
            return Shaders::VisibilityConstants{
                .mOrigin = origin,
                .mNear = near,
                .mFar = far,
                .mReach = sFarPlane,

                // Every bounce, until a world says the reconstruction follows the frame —
                // `VisibilityConstants::mBounceRate` says why a frame built by hand keeps them all.
                .mBounceRate = 1.0f,

                // No day to lift, until a world says the sun is up.
                .mDaylightGain = 1.0f,

                // Not zero, which would be sea level: a world with no water has to answer "how deep
                // is this point" with never, and only an infinity does that without a second
                // question.
                .mWaterLevel = -std::numeric_limits<float>::infinity(),

                // A sea that runs as its tiles were drawn, until a world says which way the wind
                // blows.
                .mSeaHeading = osg::Vec2f(1.0f, 0.0f),

                // The layer `FOG_HEIGHT` names, until a weather says otherwise. A camera is
                // built before anything has described the air over it, and a lift of nothing is a
                // layer of no height at all rather than an absence of one. `describeWorld` overwrites
                // this with what the cell's own weather stands its fog up to.
                .mFogLift = 1.0f,

                // Every class, until a camera with a cull mask of its own says which it draws. The
                // harness's and the tests' cameras never do.
                .mRayMask = Shaders::MASK_EVERY_CLASS,
            };
        }

        std::optional<ViewBasis> basisOf(const osg::Matrixf& view)
        {
            osg::Matrixf world;
            if (!world.invert(view))
                return std::nullopt;

            return viewBasisOf(osg::Matrixd(world));
        }
    }

    std::optional<ViewBasis> viewBasisOf(const osg::Matrixd& world)
    {
        // The rows of the inverse are the eye's axes written in world coordinates, and its
        // translation is where the eye stands.
        ViewBasis basis{
            .mOrigin = osg::Vec3f(world.getTrans()),
            .mForward = -osg::Vec3f(world(2, 0), world(2, 1), world(2, 2)),
            .mRight = osg::Vec3f(world(0, 0), world(0, 1), world(0, 2)),
            .mUp = osg::Vec3f(world(1, 0), world(1, 1), world(1, 2)),
        };

        // **Not `<= 0`, which NaN passes.** `osg::Matrixf::invert` inverts a singular view — an eye
        // looking at itself, or straight down along the up it was given — into NaN and says it
        // succeeded, and every axis of it then normalises to a length of NaN.
        if (!(basis.mForward.normalize() > 0.f) || !(basis.mRight.normalize() > 0.f) || !(basis.mUp.normalize() > 0.f)
            || basis.mOrigin.isNaN())
            return std::nullopt;

        return basis;
    }

    Shaders::Camera cameraAtFieldOfView(const Shaders::Camera& camera, const float verticalFovDegrees)
    {
        assert(camera.mOrthographic == 0 && "a parallel projection has no field of view to widen");

        const Spread spread = spreadOf(verticalFovDegrees, camera.mWidth, camera.mHeight);

        Shaders::Camera widened = camera;
        widened.mRight = camera.mRight * (spread.mHalfWidth / camera.mRight.length());
        widened.mUp = camera.mUp * (spread.mHalfHeight / camera.mUp.length());
        widened.mSpreadAngle = spread.mAngle;

        return widened;
    }

    std::optional<Shaders::VisibilityConstants> makeCameraFromView(const osg::Matrixf& view, float verticalFovDegrees,
        std::uint32_t width, std::uint32_t height, float near, float far)
    {
        assert(width > 0 && height > 0);

        const std::optional<ViewBasis> basis = basisOf(view);
        if (!basis.has_value())
            return std::nullopt;

        const Spread spread = spreadOf(verticalFovDegrees, width, height);

        Shaders::VisibilityConstants camera = beforeWorld(basis->mOrigin, near, far);
        camera.mCamera = Shaders::Camera{
            .mForward = basis->mForward,
            .mRight = basis->mRight * spread.mHalfWidth,
            .mUp = basis->mUp * spread.mHalfHeight,
            .mSpreadAngle = spread.mAngle,
            .mOrthographic = 0,
            .mWidth = width,
            .mHeight = height,
        };
        camera.mArms = camera.mCamera;

        return camera;
    }

    std::optional<Shaders::VisibilityConstants> makeOrthographicCameraFromView(const osg::Matrixf& view,
        float worldWidth, float worldHeight, std::uint32_t width, std::uint32_t height, float near, float far)
    {
        assert(width > 0 && height > 0);

        contract(worldWidth > 0.f && worldHeight > 0.f, "an orthographic camera with no extent sees nothing");

        const std::optional<ViewBasis> basis = basisOf(view);
        if (!basis.has_value())
            return std::nullopt;

        Shaders::VisibilityConstants camera = beforeWorld(basis->mOrigin, near, far);
        camera.mCamera = Shaders::Camera{
            .mForward = basis->mForward,
            .mRight = basis->mRight * (worldWidth * 0.5f),
            .mUp = basis->mUp * (worldHeight * 0.5f),

            // Zero, and not for want of an answer. A parallel ray's cone does not widen with
            // distance; what it has instead is a footprint one pixel of the box wide for its whole
            // length, which the shader works out from `mRight` rather than carry twice.
            .mSpreadAngle = 0.f,
            .mOrthographic = 1,
            .mWidth = width,
            .mHeight = height,
        };
        camera.mArms = camera.mCamera;

        return camera;
    }

    osg::Vec2f haltonJitter(std::uint32_t index)
    {
        // Counted from one, because the sequence's zeroth term is the origin — a frame that sampled
        // the pixel's corner would contribute nothing an unjittered frame did not.
        const std::uint32_t term = index + 1;

        // Centred, so the offsets straddle the pixel centre rather than filling the quadrant below
        // and to the right of it.
        return osg::Vec2f(radicalInverse(term, 2) - 0.5f, radicalInverse(term, 3) - 0.5f);
    }
}
