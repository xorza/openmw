#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <osg/Vec3f>

namespace Rtx
{
    /// One place a camera track passes through, at a frame of its take.
    struct TrackKey
    {
        /// Frames from the take's first. Strictly increasing along a track.
        std::uint32_t mFrame = 0;

        osg::Vec3f mEye;

        /// The facing, as `Stand::getRotation` gives it: `(pitch, 0, yaw)` in radians.
        osg::Vec3f mRotation;

        /// The hour of the day, from 0 up to 24.
        float mHour = 0.0f;

        /// A weather, as `Rtx::weatherIndex` numbers them.
        std::uint32_t mWeather = 0;

        /// Whether the camera stands still here, as on either side of a hold. A take's first and
        /// last keys always do.
        bool mRests = false;
    };

    /// Where a track stands at one frame, and under what.
    struct TrackPose
    {
        osg::Vec3f mEye;

        /// `(pitch, 0, yaw)` in radians, as `TrackKey::mRotation`.
        osg::Vec3f mRotation;

        /// Game hours since the take's first frame. Never less than the frame before's, since a
        /// clock asked for a lesser hour runs forward to it the next day.
        double mHoursOn = 0.0;

        /// The weather the sky leaves, the one it goes to, and how far it has crossed, nought to one.
        /// The two are the same where no crossing runs.
        std::uint32_t mWeather = 0;
        std::uint32_t mNextWeather = 0;
        float mCrossed = 0.0f;
    };

    /// The turn from `from` to `to`, in radians, the short way round: in [-π, π].
    float shortestTurn(float from, float to);

    /// How far a clock runs from `from` to `to`, in hours, always forward: in [0, 24).
    float hoursForward(float from, float to);

    /// A camera's flight through its keys, as a film's take flies it.
    ///
    /// **A cubic Hermite spline in time, per channel**: x, y and z, the yaw and the pitch, and the
    /// hour. The tangents are Catmull-Rom's, `(v₊ − v₋) / (f₊ − f₋)`, which is what a camera that
    /// must pass through every key at a given time takes, and velocity is continuous through each
    /// key. **Limited by the Fritsch–Carlson conditions**, because a Catmull-Rom tangent carries a
    /// fast segment's speed into a slow neighbour and overshoots: a flight into a pan on the spot
    /// leaves the spot and comes back. A tangent is zero where the neighbouring secants change sign
    /// or one is flat, and a segment's pair is scaled into the circle of radius three. The hour so
    /// never runs backwards, and a yaw never turns past a key.
    ///
    /// **The ends of a take and the sides of a hold rest**, so a take eases in and out: two keys
    /// alone are exactly smoothstep, `3u² − 2u³`.
    class CameraTrack
    {
    public:
        /// `keys` in order of frame, at least one, strictly increasing: a contract, since the
        /// planner that times a take is what numbers its frames.
        explicit CameraTrack(std::span<const TrackKey> keys);

        /// How many frames the take has: its last key's, and one.
        std::uint32_t getFrames() const;

        /// Where the track stands at `frame`, the last key's pose past its end.
        TrackPose pose(std::uint32_t frame) const;

    private:
        /// x, y, z, yaw, pitch, and the hours since the first key.
        static constexpr std::size_t sChannels = 6;

        struct Knot
        {
            std::uint32_t mFrame = 0;
            std::uint32_t mWeather = 0;

            /// The yaw unwrapped against the knot before and the hours counted forward from the
            /// first, so each channel is continuous.
            std::array<double, sChannels> mValue{};

            /// Each channel's rate, per frame.
            std::array<double, sChannels> mSlope{};
        };

        std::vector<Knot> mKnots;
    };
}
