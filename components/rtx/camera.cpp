#include "camera.hpp"

#include <cassert>
#include <cmath>
#include <limits>

#include <osg/Math>

#include "error.hpp"

namespace Rtx
{
    namespace
    {
        /// The `index`th term of the radical inverse in `base`, which is Halton's whole definition:
        /// write the index in that base and reflect its digits about the point.
        float radicalInverse(std::uint32_t index, std::uint32_t base)
        {
            float result = 0.0f;
            float place = 1.0f / static_cast<float>(base);

            while (index > 0)
            {
                result += static_cast<float>(index % base) * place;
                index /= base;
                place /= static_cast<float>(base);
            }

            return result;
        }

        /// A viewpoint's axes in world coordinates, which is what a view matrix holds the inverse
        /// of.
        struct ViewBasis
        {
            osg::Vec3f mOrigin;
            osg::Vec3f mForward;
            osg::Vec3f mRight;
            osg::Vec3f mUp;
        };

        ViewBasis basisOf(const osg::Matrixf& view)
        {
            osg::Matrixf world;
            if (!world.invert(view))
                throw Error("the view matrix cannot be inverted, so it names no viewpoint");

            // OpenSceneGraph's eye space is OpenGL's: +X right, +Y up and the view down -Z. The
            // rows of the inverse are those axes written in world coordinates, and its translation
            // is where the eye stands.
            ViewBasis basis{
                .mOrigin = osg::Vec3f(world(3, 0), world(3, 1), world(3, 2)),
                .mForward = -osg::Vec3f(world(2, 0), world(2, 1), world(2, 2)),
                .mRight = osg::Vec3f(world(0, 0), world(0, 1), world(0, 2)),
                .mUp = osg::Vec3f(world(1, 0), world(1, 1), world(1, 2)),
            };

            // Normalised rather than assumed: a view matrix with a scale in it is a legal one, and
            // the basis below is scaled again by the frame's own extents.
            if (basis.mForward.normalize() <= 0.f || basis.mRight.normalize() <= 0.f || basis.mUp.normalize() <= 0.f)
                throw Error("the view matrix has no basis to look along");

            return basis;
        }
    }

    Shaders::VisibilityConstants makeCameraFromView(const osg::Matrixf& view, float verticalFovDegrees,
        std::uint32_t width, std::uint32_t height, float near, float far)
    {
        assert(width > 0 && height > 0);

        const ViewBasis basis = basisOf(view);

        const float halfHeight = std::tan(osg::DegreesToRadians(verticalFovDegrees) * 0.5f);
        const float halfWidth = halfHeight * static_cast<float>(width) / static_cast<float>(height);

        return Shaders::VisibilityConstants{
            .mOrigin = basis.mOrigin,
            .mCamera = {
                .mForward = basis.mForward,
                .mRight = basis.mRight * halfWidth,
                .mUp = basis.mUp * halfHeight,
                .mSpreadAngle = std::atan(2.0f * halfHeight / static_cast<float>(height)),
                .mOrthographic = 0,
                .mWidth = width,
                .mHeight = height,
            },
            .mNear = near,
            .mFar = far,
            .mWaterLevel = -std::numeric_limits<float>::infinity(),
        };
    }

    Shaders::VisibilityConstants makeOrthographicCameraFromView(const osg::Matrixf& view, float worldWidth,
        float worldHeight, std::uint32_t width, std::uint32_t height, float near, float far)
    {
        assert(width > 0 && height > 0);

        if (!(worldWidth > 0.f) || !(worldHeight > 0.f))
            throw Error("an orthographic camera with no extent sees nothing");

        const ViewBasis basis = basisOf(view);

        return Shaders::VisibilityConstants{
            .mOrigin = basis.mOrigin,
            .mCamera = {
                .mForward = basis.mForward,
                .mRight = basis.mRight * (worldWidth * 0.5f),
                .mUp = basis.mUp * (worldHeight * 0.5f),
                .mSpreadAngle = 0.f,
                .mOrthographic = 1,
                .mWidth = width,
                .mHeight = height,
            },
            .mNear = near,
            .mFar = far,
            // **Zero, and not for want of an answer.** A parallel ray's cone does not widen with
            // distance; what it has instead is a footprint one pixel of the box wide for its whole
            // length, which the shader works out from `mRight` rather than carry twice.
            .mWaterLevel = -std::numeric_limits<float>::infinity(),
        };
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

    Shaders::VisibilityConstants makeCamera(const osg::Vec3f& origin, const osg::Vec3f& target,
        float verticalFovDegrees, std::uint32_t width, std::uint32_t height, float far)
    {
        const osg::Vec3f along = target - origin;
        if (along.length2() <= 0.0f)
            throw Error("the camera is standing where it is looking");

        return makeCameraAlong(origin, along, verticalFovDegrees, width, height, far);
    }

    Shaders::VisibilityConstants makeCameraAlong(const osg::Vec3f& origin, const osg::Vec3f& along,
        float verticalFovDegrees, std::uint32_t width, std::uint32_t height, float far)
    {
        assert(width > 0 && height > 0);

        osg::Vec3f forward = along;
        if (forward.length2() <= 0.0f)
            throw Error("the camera has no direction to look along");
        forward.normalize();

        const osg::Vec3f worldUp(0.0f, 0.0f, 1.0f);
        osg::Vec3f right = forward ^ worldUp;
        if (right.length2() <= 1e-6f)
            throw Error("the camera looks along the world's up axis, which leaves its roll undefined");
        right.normalize();

        osg::Vec3f up = right ^ forward;
        up.normalize();

        const float halfHeight = std::tan(osg::DegreesToRadians(verticalFovDegrees) * 0.5f);
        const float halfWidth = halfHeight * static_cast<float>(width) / static_cast<float>(height);

        // The vertical angle one pixel covers. Pixels are square here, so one number does for both.
        const float spread = std::atan(2.0f * halfHeight / static_cast<float>(height));

        return Shaders::VisibilityConstants{
            .mOrigin = origin,
            .mCamera = {
                .mForward = forward,
                .mRight = right * halfWidth,
                .mUp = up * halfHeight,
                .mSpreadAngle = spread,
                .mOrthographic = 0,
                .mWidth = width,
                .mHeight = height,
            },
            // A quarter of a Morrowind foot. Nothing is clipped against it — see `mNear` — so it
            // only has to be nearer than anything the eye can find itself inside of.
            .mNear = 1.0f,
            .mFar = far,
            // Not zero, which would be sea level: a world with no water has to answer "how deep is
            // this point" with never, and only an infinity does that without a second question.
            .mWaterLevel = -std::numeric_limits<float>::infinity(),
        };
    }
}
