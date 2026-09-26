#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Vec3f>

#include <apps/rtxtool/model/cameratrack.hpp>

namespace RtxTool
{
    namespace
    {
        constexpr float sDegree = std::numbers::pi_v<float> / 180.0f;

        TrackKey keyAt(std::uint32_t frame, float x, float yawDegrees = 0.0f, float hour = 12.0f,
            std::uint32_t weather = 0, bool rests = false)
        {
            return TrackKey{ .mFrame = frame,
                .mEye = osg::Vec3f(x, 0.0f, 0.0f),
                .mRotation = osg::Vec3f(0.0f, 0.0f, yawDegrees * sDegree),
                .mHour = hour,
                .mWeather = weather,
                .mRests = rests };
        }

        /// Two keys are exactly smoothstep, `3u² − 2u³`: at u = 0.2 that is 0.12 − 0.016 = 0.104,
        /// and at a half, a half. The last key's pose stands past the end.
        TEST(RtxCameraTrackTest, twoKeysEaseInAndOutBySmoothstep)
        {
            const std::vector<TrackKey> keys{ keyAt(0, 0.0f), keyAt(10, 100.0f) };
            const CameraTrack track(keys);

            EXPECT_EQ(track.getFrames(), 11u);
            EXPECT_FLOAT_EQ(track.pose(0).mEye.x(), 0.0f);
            EXPECT_FLOAT_EQ(track.pose(2).mEye.x(), 10.4f);
            EXPECT_FLOAT_EQ(track.pose(5).mEye.x(), 50.0f);
            EXPECT_FLOAT_EQ(track.pose(10).mEye.x(), 100.0f);
            EXPECT_FLOAT_EQ(track.pose(20).mEye.x(), 100.0f);

            const std::vector<TrackKey> one{ keyAt(0, 7.0f) };
            const CameraTrack still(one);
            EXPECT_EQ(still.getFrames(), 1u);
            EXPECT_FLOAT_EQ(still.pose(3).mEye.x(), 7.0f);
        }

        /// Three keys ten frames and a hundred units apart: Catmull-Rom's tangent at the middle is
        /// 200 / 20 = 10 a frame. At frame 15, u = 0.5: 0.5·100 + 0.125·10·10 + 0.5·200 = 162.5. At
        /// frames 9 and 11 the curve is 89.1 and 110.9, the same 10.9 either side of the key, so the
        /// camera does not change speed through it. A resting key has no tangent: 97.2 and 102.8,
        /// which is the smoothstep of each side on its own.
        TEST(RtxCameraTrackTest, aKeyIsPassedAtOneSpeedUnlessItRests)
        {
            const std::vector<TrackKey> keys{ keyAt(0, 0.0f), keyAt(10, 100.0f), keyAt(20, 200.0f) };
            const CameraTrack track(keys);

            EXPECT_FLOAT_EQ(track.pose(10).mEye.x(), 100.0f);
            EXPECT_FLOAT_EQ(track.pose(15).mEye.x(), 162.5f);
            EXPECT_NEAR(track.pose(9).mEye.x(), 89.1f, 1e-4f);
            EXPECT_NEAR(track.pose(11).mEye.x(), 110.9f, 1e-4f);

            const std::vector<TrackKey> resting{ keyAt(0, 0.0f), keyAt(10, 100.0f, 0.0f, 12.0f, 0, true),
                keyAt(20, 200.0f) };
            const CameraTrack held(resting);
            EXPECT_NEAR(held.pose(9).mEye.x(), 97.2f, 1e-4f);
            EXPECT_NEAR(held.pose(11).mEye.x(), 102.8f, 1e-4f);
        }

        /// A flight into a pan on the spot: Catmull-Rom's tangent at the spot is 1000 / 40 = 25 a
        /// frame, which would carry the camera past it and back. The pan's secant is flat, so the
        /// tangent is none and the camera stands exactly at the spot for the whole pan, while the
        /// yaw turns through it.
        TEST(RtxCameraTrackTest, aPanOnTheSpotDoesNotLeaveTheSpot)
        {
            const std::vector<TrackKey> keys{ keyAt(0, 0.0f), keyAt(10, 1000.0f), keyAt(40, 1000.0f, 90.0f) };
            const CameraTrack track(keys);

            for (std::uint32_t frame = 10; frame <= 40; ++frame)
                EXPECT_FLOAT_EQ(track.pose(frame).mEye.x(), 1000.0f) << "frame " << frame;

            for (std::uint32_t frame = 0; frame < 10; ++frame)
                EXPECT_LE(track.pose(frame).mEye.x(), 1000.0f) << "frame " << frame;

            EXPECT_NEAR(track.pose(25).mRotation.z(), 45.0f * sDegree, 1e-5f);
        }

        /// From 170° to −170° is twenty degrees east through south, not 340 back through north:
        /// halfway is due south, and no frame faces north of east or west.
        TEST(RtxCameraTrackTest, aTurnGoesTheShortWay)
        {
            EXPECT_NEAR(shortestTurn(170.0f * sDegree, -170.0f * sDegree), 20.0f * sDegree, 1e-6f);
            EXPECT_NEAR(shortestTurn(0.0f, 270.0f * sDegree), -90.0f * sDegree, 1e-6f);
            EXPECT_NEAR(shortestTurn(-10.0f * sDegree, 10.0f * sDegree), 20.0f * sDegree, 1e-6f);

            const std::vector<TrackKey> keys{ keyAt(0, 0.0f, 170.0f), keyAt(10, 0.0f, -170.0f) };
            const CameraTrack track(keys);

            EXPECT_NEAR(std::cos(track.pose(5).mRotation.z()), -1.0f, 1e-6f);
            for (std::uint32_t frame = 0; frame <= 10; ++frame)
                EXPECT_LT(std::cos(track.pose(frame).mRotation.z()), -0.98f) << "frame " << frame;
        }

        /// 22:00 to 02:00 is four hours forward through midnight, so halfway is two hours on. A key
        /// at the same hour runs none, and a lesser hour runs to it the next day.
        TEST(RtxCameraTrackTest, theClockRunsForwardThroughMidnight)
        {
            EXPECT_FLOAT_EQ(hoursForward(5.0f, 5.0f), 0.0f);
            EXPECT_FLOAT_EQ(hoursForward(23.5f, 0.5f), 1.0f);
            EXPECT_FLOAT_EQ(hoursForward(1.0f, 23.0f), 22.0f);

            const std::vector<TrackKey> keys{ keyAt(0, 0.0f, 0.0f, 22.0f), keyAt(10, 0.0f, 0.0f, 2.0f) };
            const CameraTrack track(keys);
            EXPECT_DOUBLE_EQ(track.pose(0).mHoursOn, 0.0);
            EXPECT_DOUBLE_EQ(track.pose(5).mHoursOn, 2.0);
            EXPECT_DOUBLE_EQ(track.pose(10).mHoursOn, 4.0);
        }

        /// 06:00, 07:00, 18:00, 18:00 at frames 0, 10, 20 and 60. Catmull-Rom's tangent at 07:00 is
        /// 12 / 20 = 0.6 an hour a frame against a secant of 0.1 before it, and the first segment
        /// would then run 4u³ − 3u² hours: a quarter of an hour backwards at u = 0.5. The circle of
        /// radius three scales the tangent by 3 / 6 to 0.3, and the segment runs u³ hours: an
        /// eighth at frame 5. No frame runs the clock backwards.
        TEST(RtxCameraTrackTest, theClockNeverRunsBackwards)
        {
            const std::vector<TrackKey> keys{ keyAt(0, 0.0f, 0.0f, 6.0f), keyAt(10, 0.0f, 0.0f, 7.0f),
                keyAt(20, 0.0f, 0.0f, 18.0f), keyAt(60, 0.0f, 0.0f, 18.0f) };
            const CameraTrack track(keys);

            EXPECT_NEAR(track.pose(5).mHoursOn, 0.125, 1e-12);
            for (std::uint32_t frame = 0; frame < 60; ++frame)
                EXPECT_LE(track.pose(frame).mHoursOn, track.pose(frame + 1).mHoursOn) << "frame " << frame;
            EXPECT_DOUBLE_EQ(track.pose(40).mHoursOn, 12.0);
        }

        /// A crossing of the sky eases as the camera does: smoothstep of 0.2 is 0.104. At a key the
        /// crossing into the next weather has not begun, and at the last the sky has arrived.
        TEST(RtxCameraTrackTest, theSkyCrossesBetweenTwoKeysWeathers)
        {
            const std::vector<TrackKey> keys{ keyAt(0, 0.0f, 0.0f, 12.0f, 0), keyAt(10, 0.0f, 0.0f, 12.0f, 5),
                keyAt(20, 0.0f, 0.0f, 12.0f, 5) };
            const CameraTrack track(keys);

            const TrackPose start = track.pose(0);
            EXPECT_EQ(start.mWeather, 0u);
            EXPECT_EQ(start.mNextWeather, 5u);
            EXPECT_FLOAT_EQ(start.mCrossed, 0.0f);

            EXPECT_FLOAT_EQ(track.pose(2).mCrossed, 0.104f);
            EXPECT_FLOAT_EQ(track.pose(5).mCrossed, 0.5f);

            const TrackPose steady = track.pose(15);
            EXPECT_EQ(steady.mWeather, 5u);
            EXPECT_EQ(steady.mNextWeather, 5u);
            EXPECT_FLOAT_EQ(steady.mCrossed, 0.0f);

            const TrackPose end = track.pose(20);
            EXPECT_EQ(end.mWeather, 5u);
            EXPECT_EQ(end.mNextWeather, 5u);
        }
    }
}
