#pragma once

#include <cstddef>
#include <cstdint>

#include <osg/Vec3f>

namespace RtxTool
{
    struct Route;
    struct Stop;

    /// Moves the camera through a stop, frame by frame: where a route has flown to, where a film's
    /// track stands the eye, the clock and the sky, which weather a turning sky is on, and the aim
    /// that holds the camera there. What the world is put in once, at the stop's start, is
    /// `Stager`'s; this starts where that leaves the player.
    class CameraDriver
    {
    public:
        /// Starts `stop`'s camera where the stager left the player: at the stop's eye, facing its look,
        /// or where the player stands where the stop names no eye or hands the camera to the player.
        /// A track's clock runs from the game's time now. Aims the camera, because a frame drawn
        /// before the first `aim` would be drawn from wherever the last stop left it.
        void begin(const Stop& stop);

        /// Moves one frame of `stop`, the `seen`th since it began, of which `warmup` are thrown
        /// away: a track stands its eye, clock and sky at every frame, the warm-up at its first; a
        /// route and a turning sky move over the measured frames alone, `seconds` of world a
        /// frame.
        void step(const Stop& stop, std::uint32_t seen, std::uint32_t warmup, float seconds);

        /// Puts the camera where the stop stands this frame, and points it where the stop asked.
        /// Every frame, because `omw/camera/camera.lua`'s `onActive` forces third person and a
        /// stop's teleport reactivates the player: aimed once, every view drew its frames from a
        /// camera 192 units behind the body. `Check::CameraStands` says it still holds. A standing
        /// stop and a heading route are one case, carrying the heading forward from where the route
        /// has flown; nothing where the stop named no camera or gave it to the player.
        void aim(const Stop& stop);

        /// Whether the route has reached the destination it named. What ends a routed stop: the
        /// frames past arrival stand where the route ended and measure nothing the route was flown
        /// for, so `--seconds` is the ceiling a route that never arrives runs to.
        bool hasArrived() const { return mArrived; }

        /// How much of `route`'s line has been flown, nought to one, and one for a line of no
        /// length: a run that ended short measured a shorter journey than its name says.
        float getTravelled(const Route& route) const;

    private:
        /// Puts the route where it starts, the one place all three are set.
        void standAt(const osg::Vec3f& eye, const osg::Vec3f& look);

        /// Puts the camera where the player stands, facing the way they face: what a stop that names
        /// no camera falls back to, and what a free-camera stop starts from.
        void standWhereThePlayerIs();

        /// Flies the player along `route` by one frame's worth of `step` seconds.
        void fly(const Route& route, float step);

        /// Moves the sky one frame of `step` seconds along `through`.
        void turnWeather(const Stop& stop, float step);

        /// Puts the player's body at `eye`, where a route or a track has the camera this frame.
        static void moveBodyTo(const osg::Vec3f& eye);

        /// Stands the game's camera at `eye` facing `rotation`, `Stand::getRotation`'s angles, for
        /// as long as nothing else moves it.
        static void aimCamera(const osg::Vec3f& eye, const osg::Vec3f& rotation);

        /// Where the eye stood when the stop began, which a route flies from.
        osg::Vec3f mFrom;
        osg::Vec3f mFromLook;

        /// Where the route has flown to: the route's own place and not the player's, because
        /// gravity steps the actor between frames and a step taken from where it landed compounds
        /// the fall.
        osg::Vec3f mFlown;

        /// Which way a track faces this frame, as `TrackPose::mRotation`.
        osg::Vec3f mFacing;

        /// The game's clock at a track's first frame, in hours since the game began: what the
        /// track's hours run on from.
        double mClockFrom = 0.0;

        bool mArrived = false;

        /// Which weather the turn is on, and how far into the transition to the next.
        std::size_t mTurnedTo = 0;
        float mTurned = 0.0f;
    };
}
