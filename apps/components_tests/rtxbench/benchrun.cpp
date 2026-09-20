#include <span>

#include <gtest/gtest.h>

#include <osg/Math>
#include <osg/Vec3f>

#include <components/rtx/reconstruction.hpp>
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

        /// **Every check has a row, and the row says when it may be asked.** A check with no name
        /// would print empty in the report and be unreachable from the command line; the four that
        /// depend on the stop's shape answer no where the stop cannot answer them.
        TEST(RtxBenchRunTest, everyCheckIsNamedAndSaysWhenItMayBeAsked)
        {
            const std::span<const Check> every = everyCheck();
            EXPECT_EQ(every.size(), 13u);
            for (const Check check : every)
                EXPECT_FALSE(checkName(check).empty()) << static_cast<int>(check);

            Stop still;
            still.mStand.mEye = osg::Vec3f(1.0f, 2.0f, 3.0f);
            RenderProfile unheld;
            RenderProfile held = unheld;
            held.mStressOverlapMs = 8.0;

            EXPECT_TRUE(canAsk(Check::WalkTwice, still, unheld));
            EXPECT_TRUE(canAsk(Check::CameraStands, still, unheld)) << "a still stop names its eye";
            EXPECT_FALSE(canAsk(Check::CrossingsAppend, still, unheld)) << "nothing to cross without a route";
            EXPECT_TRUE(canAsk(Check::FramesOverlap, still, unheld));
            EXPECT_FALSE(canAsk(Check::QueueHeld, still, unheld)) << "a hold nobody asked for";
            EXPECT_TRUE(canAsk(Check::QueueHeld, still, held));
            EXPECT_TRUE(canAsk(Check::DriverQuiet, still, unheld)) << "asked of every stop, short ones by their count";

            Stop routed = still;
            routed.mSchedule.mRoute = Route{ .mTo = osg::Vec3f(100.0f, 0.0f, 0.0f), .mSpeed = 10.0f };
            EXPECT_TRUE(canAsk(Check::CrossingsAppend, routed, unheld));
            EXPECT_FALSE(canAsk(Check::FramesOverlap, routed, unheld)) << "an arrival drains the ring";
            EXPECT_FALSE(canAsk(Check::CameraStands, routed, unheld)) << "a route leaves the camera elsewhere";

            Stop flown = still;
            flown.mSchedule.mFreeCamera = true;
            EXPECT_FALSE(canAsk(Check::CameraStands, flown, unheld));

            Stop unplaced;
            EXPECT_FALSE(canAsk(Check::CameraStands, unplaced, unheld)) << "no eye named, nothing to stand at";
        }
    }
}
