#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

#include <osg/Matrixf>
#include <osg/Vec2f>
#include <osg/Vec3f>

#include <components/rtx/camera.hpp>
#include <components/rtx/shaders/visibility.h>

#include "support/death.hpp"
#include "support/testcamera.hpp"

namespace Rtx
{
    namespace
    {
        TEST(RtxCameraTest, theBasisIsRightHandedAboutTheWorldsUpAxis)
        {
            // Looking along +Y from the origin, 90 degrees of vertical field of view, square image:
            // the half-extents at unit distance are both tan(45) = 1.
            const Shaders::VisibilityConstants camera = Testing::makeCamera(
                osg::Vec3f(0.0f, 0.0f, 0.0f), osg::Vec3f(0.0f, 1.0f, 0.0f), 90.0f, 100, 100, 1000.0f);

            EXPECT_NEAR(camera.mCamera.mForward.y(), 1.0f, 1e-5f);
            EXPECT_NEAR(camera.mCamera.mRight.x(), 1.0f, 1e-5f);
            EXPECT_NEAR(camera.mCamera.mUp.z(), 1.0f, 1e-5f);
        }

        TEST(RtxCameraTest, aWiderImageWidensTheHorizontalExtentAndLeavesTheVerticalAlone)
        {
            const Shaders::VisibilityConstants wide = Testing::makeCamera(
                osg::Vec3f(0.0f, 0.0f, 0.0f), osg::Vec3f(0.0f, 1.0f, 0.0f), 90.0f, 200, 100, 1000.0f);

            EXPECT_NEAR(wide.mCamera.mRight.x(), 2.0f, 1e-5f);
            EXPECT_NEAR(wide.mCamera.mUp.z(), 1.0f, 1e-5f);
        }

        /// A view with no basis is nothing rather than a camera of NaN: a camera nobody filled in
        /// arrives every frame, and a frame skips rather than filling the image with NaN and
        /// reporting nothing. An eye looking at itself inverts to no matrix, and one looking
        /// straight down with the world's up for its roll has no right-hand side.
        TEST(RtxCameraTest, aViewWithNoBasisIsNothingRatherThanNaN)
        {
            const osg::Vec3f eye(1.0f, 2.0f, 3.0f);
            const osg::Vec3f up(0.0f, 0.0f, 1.0f);
            EXPECT_FALSE(
                makeCameraFromView(osg::Matrixf::lookAt(eye, eye, up), 60.0f, 64, 64, sNearPlane, 1.0f).has_value());

            const osg::Vec3f above(0.0f, 0.0f, 100.0f);
            EXPECT_FALSE(
                makeCameraFromView(osg::Matrixf::lookAt(above, osg::Vec3f(), up), 60.0f, 64, 64, sNearPlane, 1.0f)
                    .has_value());
        }

        /// Straight down, the one viewpoint a map has, with the roll `lookAt`'s own up gives it.
        ///
        /// The extents are the box in world units and not an angle: half of two hundred across and
        /// half of a hundred down, on the axes `lookAt` puts them.
        TEST(RtxCameraTest, anOrthographicCameraCarriesItsBoxRatherThanAFieldOfView)
        {
            const osg::Matrixf view
                = osg::Matrixf::lookAt(osg::Vec3f(0.0f, 0.0f, 100.0f), osg::Vec3f(), osg::Vec3f(0.0f, 1.0f, 0.0f));

            const Shaders::VisibilityConstants camera
                = makeOrthographicCameraFromView(view, 200.0f, 100.0f, 64, 32, 5.0f, 400.0f).value();

            EXPECT_EQ(camera.mCamera.mOrthographic, 1u);

            EXPECT_NEAR(camera.mOrigin.z(), 100.0f, 1e-4f);
            EXPECT_NEAR(camera.mCamera.mForward.z(), -1.0f, 1e-5f);
            EXPECT_NEAR(camera.mCamera.mRight.x(), 100.0f, 1e-4f);
            EXPECT_NEAR(camera.mCamera.mUp.y(), 50.0f, 1e-4f);

            // No angle, because a parallel ray's cone does not widen; the shader takes the pixel's
            // constant footprint off `mRight` instead.
            EXPECT_EQ(camera.mCamera.mSpreadAngle, 0.0f);

            Testing::expectDies([&] { makeOrthographicCameraFromView(view, 0.0f, 100.0f, 64, 32, 5.0f, 400.0f); },
                "an orthographic camera with no extent sees nothing");
        }

        /// **The two builders agree on everything a camera carries that is not its own basis.** A
        /// viewpoint is built before anything has described the world over it, and what the two
        /// leave behind for `describeWorld` to overwrite has to be one answer — a sea level of
        /// never, a heading the tiles were drawn on, and a fog layer of the height `FOG_HEIGHT`
        /// names.
        ///
        /// **The clip is the caller's and the reach is the world's.** A picture that clips at four
        /// hundred units still sends its shadow and ambient rays to `sFarPlane`, because what
        /// lights a point is the world around it and not how near a picture of it stops.
        TEST(RtxCameraTest, everyBuilderLeavesTheSameWorldBehindIt)
        {
            const osg::Vec3f eye(0.0f, 0.0f, 100.0f);
            const osg::Matrixf view = osg::Matrixf::lookAt(eye, osg::Vec3f(), osg::Vec3f(0.0f, 1.0f, 0.0f));

            const std::array cameras{
                makeCameraFromView(view, 60.0f, 64, 32, 1.0f, 400.0f).value(),
                makeOrthographicCameraFromView(view, 200.0f, 100.0f, 64, 32, 1.0f, 400.0f).value(),
            };

            for (const Shaders::VisibilityConstants& camera : cameras)
            {
                EXPECT_EQ(camera.mWaterLevel, -std::numeric_limits<float>::infinity());
                EXPECT_EQ(camera.mSeaHeading, osg::Vec2f(1.0f, 0.0f));
                EXPECT_EQ(camera.mFogLift, 1.0f);
                EXPECT_EQ(camera.mFar, 400.0f);
                EXPECT_EQ(camera.mReach, sFarPlane);
                EXPECT_EQ(camera.mNear, 1.0f);
            }
        }

        /// **The image plane is the field of view over the extent.** Hand-computed at 90 degrees
        /// over 200 by 100: the half-height is `tan(45°)` — one — the half-width is that times the
        /// aspect, which is two, and one pixel covers `atan(2 / 100)` radians.
        TEST(RtxCameraTest, theImagePlaneIsTheFieldOfViewOverTheExtent)
        {
            const osg::Vec3f eye(3.0f, 4.0f, 5.0f);
            const osg::Matrixf view
                = osg::Matrixf::lookAt(eye, eye + osg::Vec3f(0.0f, 1.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 1.0f));
            const Shaders::VisibilityConstants viewed
                = makeCameraFromView(view, 90.0f, 200, 100, 1.0f, 1000.0f).value();

            EXPECT_NEAR(viewed.mCamera.mRight.length(), 2.0f, 1e-5f);
            EXPECT_NEAR(viewed.mCamera.mUp.length(), 1.0f, 1e-5f);
            EXPECT_NEAR(viewed.mCamera.mSpreadAngle, std::atan(2.0f / 100.0f), 1e-6f);
        }

        /// The arms' eye is the eye's own until something widens it, and widening keeps the basis
        /// and moves the plane.
        ///
        /// Ninety degrees over 200 by 100 is the plane `theImagePlaneIsTheFieldOfViewOverTheExtent`
        /// works out — half-height one, half-width two, `atan(2 / 100)` a pixel — reached here
        /// from a sixty-degree camera whose own half-height is `tan(30°)`.
        TEST(RtxCameraTest, theArmsEyeIsTheEyesOwnUntilWidened)
        {
            const osg::Vec3f eye(3.0f, 4.0f, 5.0f);
            const osg::Vec3f along(0.0f, 1.0f, 0.0f);
            const osg::Matrixf view = osg::Matrixf::lookAt(eye, eye + along, osg::Vec3f(0.0f, 0.0f, 1.0f));

            for (const Shaders::VisibilityConstants& built :
                { makeCameraFromView(view, 60.0f, 200, 100, 1.0f, 1000.0f).value(),
                    makeOrthographicCameraFromView(view, 200.0f, 100.0f, 200, 100, 1.0f, 1000.0f).value() })
            {
                EXPECT_EQ(built.mArms.mForward, built.mCamera.mForward);
                EXPECT_EQ(built.mArms.mRight, built.mCamera.mRight);
                EXPECT_EQ(built.mArms.mUp, built.mCamera.mUp);
                EXPECT_EQ(built.mArms.mSpreadAngle, built.mCamera.mSpreadAngle);
                EXPECT_EQ(built.mArms.mWidth, built.mCamera.mWidth);
            }

            const Shaders::VisibilityConstants narrow
                = makeCameraFromView(view, 60.0f, 200, 100, 1.0f, 1000.0f).value();
            const Shaders::Camera wide = cameraAtFieldOfView(narrow.mCamera, 90.0f);

            EXPECT_EQ(wide.mForward, narrow.mCamera.mForward);
            EXPECT_NEAR(wide.mRight.length(), 2.0f, 1e-5f);
            EXPECT_NEAR(wide.mUp.length(), 1.0f, 1e-5f);
            EXPECT_NEAR(wide.mSpreadAngle, std::atan(2.0f / 100.0f), 1e-6f);
            EXPECT_EQ(wide.mWidth, 200u);
            EXPECT_EQ(wide.mHeight, 100u);

            // The same axes, only longer.
            for (int axis = 0; axis < 3; ++axis)
            {
                EXPECT_NEAR(wide.mRight[axis] / wide.mRight.length(),
                    narrow.mCamera.mRight[axis] / narrow.mCamera.mRight.length(), 1e-6f)
                    << "right " << axis;
                EXPECT_NEAR(
                    wide.mUp[axis] / wide.mUp.length(), narrow.mCamera.mUp[axis] / narrow.mCamera.mUp.length(), 1e-6f)
                    << "up " << axis;
            }
        }

        /// Halton, against its own definition worked out by hand.
        ///
        /// The radical inverse writes an index in a base and reflects its digits about the point, so
        /// term one in base two is 0.1 binary and term two is 0.01 — a half and a quarter. Base
        /// three's first three are a third, two thirds and a ninth. Centring subtracts a half from
        /// each, and the sequence is counted from one because term zero is the origin: a frame that
        /// sampled the pixel's corner would tell an upscaler nothing an unjittered one did not.
        TEST(RtxJitterTest, theSequenceIsHaltonInTwoAndThreeAndStraddlesTheCentre)
        {
            EXPECT_NEAR(haltonJitter(0).x(), 0.0f, 1e-6f) << "1/2 - 1/2";
            EXPECT_NEAR(haltonJitter(1).x(), -0.25f, 1e-6f) << "1/4 - 1/2";
            EXPECT_NEAR(haltonJitter(2).x(), 0.25f, 1e-6f) << "3/4 - 1/2";
            EXPECT_NEAR(haltonJitter(3).x(), -0.375f, 1e-6f) << "1/8 - 1/2";

            EXPECT_NEAR(haltonJitter(0).y(), 1.0f / 3.0f - 0.5f, 1e-6f);
            EXPECT_NEAR(haltonJitter(1).y(), 2.0f / 3.0f - 0.5f, 1e-6f);
            EXPECT_NEAR(haltonJitter(2).y(), 1.0f / 9.0f - 0.5f, 1e-6f);

            // Inside the pixel, every term, which is what makes it a sub-pixel offset rather than a
            // camera shake.
            for (std::uint32_t index = 0; index < 64; ++index)
            {
                const osg::Vec2f at = haltonJitter(index);
                EXPECT_GE(at.x(), -0.5f);
                EXPECT_LT(at.x(), 0.5f);
                EXPECT_GE(at.y(), -0.5f);
                EXPECT_LT(at.y(), 0.5f);
            }
        }
    }
}
