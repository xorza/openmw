#include <gtest/gtest.h>

#include <osg/Math>
#include <osg/Vec3f>

#include <components/rtxbench/benchrun.hpp>

namespace Rtx
{
    namespace
    {
        /// The rotation a stand hands the game's body, in the game's own angles.
        TEST(RtxBenchRunTest, aStandsRotationIsTheBodysYawClockwiseFromNorthAndPitchNegativeUp)
        {
            const osg::Vec3f eye(10.0f, 20.0f, 30.0f);

            const Stand north{ .mEye = eye, .mLook = eye + osg::Vec3f(0.0f, 100.0f, 0.0f) };
            EXPECT_EQ(north.getRotation(), osg::Vec3f(0.0f, 0.0f, 0.0f));

            const Stand east{ .mEye = eye, .mLook = eye + osg::Vec3f(100.0f, 0.0f, 0.0f) };
            EXPECT_NEAR(east.getRotation().z(), osg::PI_2, 1e-6);
            EXPECT_NEAR(east.getRotation().x(), 0.0f, 1e-6f);

            const Stand up{ .mEye = eye, .mLook = eye + osg::Vec3f(0.0f, 0.0f, 100.0f) };
            EXPECT_NEAR(up.getRotation().x(), -osg::PI_2, 1e-6);

            // A stand with no look faces north, as `getLook` says.
            const Stand bare{ .mEye = eye };
            EXPECT_EQ(bare.getRotation(), osg::Vec3f(0.0f, 0.0f, 0.0f));
        }

        /// The ship at Seyda Neen, by hand: forward (-4411, 2767, -120) is 5208.4 long, so the yaw
        /// is atan2(-4411, 2767) = -1.01055 rad (302.1° from north) and the pitch is
        /// -asin(-120 / 5208.4) = +0.02304 rad, a climb of -1.32°.
        TEST(RtxBenchRunTest, theShipsStandFacesTheTownAndDipsALittle)
        {
            const Stand ship{
                .mEye = osg::Vec3f(-8292.0f, -73376.0f, 320.0f),
                .mLook = osg::Vec3f(-12703.0f, -70609.0f, 200.0f),
            };
            const osg::Vec3f rotation = ship.getRotation();

            EXPECT_NEAR(rotation.z(), -1.01055f, 1e-4f);
            EXPECT_NEAR(rotation.x(), 0.02304f, 1e-4f);
            EXPECT_EQ(rotation.y(), 0.0f);
        }

        /// What `RtxTool::Session::standWhereThePlayerIs` rebuilds from a body's rotation is the
        /// stand that rotated it. The ship again, so the two are checked on the same numbers:
        /// forward is (-4411, 2767, -120) / 5208.4 = (-0.8469, 0.5313, -0.0230).
        TEST(RtxBenchRunTest, aBodysRotationReadsBackAsTheStandThatMadeIt)
        {
            const Stand stand{
                .mEye = osg::Vec3f(-8292.0f, -73376.0f, 320.0f),
                .mLook = osg::Vec3f(-12703.0f, -70609.0f, 200.0f),
            };
            const osg::Vec3f forward = Stand::forwardOf(stand.getRotation());

            EXPECT_NEAR(forward.x(), -0.8469f, 1e-4f);
            EXPECT_NEAR(forward.y(), 0.5313f, 1e-4f);
            EXPECT_NEAR(forward.z(), -0.0230f, 1e-4f);
            EXPECT_NEAR(forward.length(), 1.0f, 1e-6f);

            EXPECT_EQ(Stand::forwardOf(osg::Vec3f()), osg::Vec3f(0.0f, 1.0f, 0.0f));
        }
    }
}
