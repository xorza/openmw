#include "cameradriver.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include <osg/Vec3d>

#include <apps/openmw/mwbase/environment.hpp>
#include <apps/openmw/mwbase/world.hpp>
#include <apps/openmw/mwrender/camera.hpp>
#include <apps/openmw/mwrender/renderingmanager.hpp>
#include <apps/openmw/mwworld/datetimemanager.hpp>
#include <apps/openmw/mwworld/ptr.hpp>
#include <apps/openmw/mwworld/timestamp.hpp>
#include <components/esm/position.hpp>
#include <components/esm3/loadregn.hpp>

#include "model/benchrun.hpp"
#include "model/cameratrack.hpp"
#include "run.hpp"
#include "stager.hpp"

namespace RtxTool
{
    void CameraDriver::begin(const Stop& stop)
    {
        *this = CameraDriver{};

        if (stop.mSchedule.mFreeCamera || !stop.mStand.mEye.has_value())
        {
            standWhereThePlayerIs();
            return;
        }

        if (stop.mSchedule.mTrack.has_value())
        {
            const MWWorld::TimeStamp now = MWBase::Environment::get().getWorld()->getTimeStamp();
            mClockFrom = now.getDay() * 24.0 + now.getHour();
            mFacing = stop.mStand.getRotation();
        }

        standAt(*stop.mStand.mEye, stop.mStand.getLook());
        aim(stop);
    }

    void CameraDriver::step(const Stop& stop, const std::uint32_t seen, const std::uint32_t warmup, const float seconds)
    {
        if (stop.mSchedule.mTrack.has_value())
        {
            // The frame about to be drawn is the take's `seen - warmup`th, counted from nought,
            // since the measurer counts it before it measures it.
            const TrackPose pose = stop.mSchedule.mTrack->pose(seen > warmup ? seen - warmup : 0);

            mFlown = pose.mEye;
            mFacing = pose.mRotation;
            moveBodyTo(pose.mEye);

            MWBase::World& world = *MWBase::Environment::get().getWorld();
            world.holdWeather(ESM::Weather::indexToRefId(static_cast<int>(pose.mWeather)),
                ESM::Weather::indexToRefId(static_cast<int>(pose.mNextWeather)), pose.mCrossed);

            // **Run forward to the track's hour and never set to it**, because only an advance keeps
            // the day, the month and the days passed in step with the hour, which the moons read.
            const MWWorld::TimeStamp now = world.getTimeStamp();
            const double behind = mClockFrom + pose.mHoursOn - (now.getDay() * 24.0 + now.getHour());
            if (behind > 0.0)
                world.advanceTime(behind, true);
            return;
        }

        // **The route runs over the measured frames and not the warm-up.** Warming up is the GPU
        // coming off its idle clock; flying during it would start the measurement partway along
        // and leave the first crossing outside the numbers.
        if (seen < warmup)
            return;

        if (stop.mSchedule.mRoute.has_value())
            fly(*stop.mSchedule.mRoute, seconds);

        turnWeather(stop, seconds);
    }

    void CameraDriver::fly(const Route& route, const float step)
    {
        // **Measured from `mFlown` and never from the player**, which says why.
        osg::Vec3f along = route.mTo - mFlown;
        const float left = along.length();
        if (left <= 0.0f)
        {
            mArrived = true;
            return;
        }

        along.normalize();

        // **Off the frame index and not the clock**, for the reason the world is stepped that way:
        // a camera advanced by how long the last frame took crosses its boundaries somewhere else
        // on every machine, and where they fall is the whole measurement. The height is the line's
        // between the two ends, which is what a view states when it names both.
        mFlown += along * std::min(route.mSpeed * step, left);
        moveBodyTo(mFlown);
    }

    void CameraDriver::turnWeather(const Stop& stop, const float step)
    {
        const std::vector<std::string>& through = stop.mSky.mTurnThrough;
        if (through.size() < 2)
            return;

        // **How often a run that turns its sky asks for the next weather, by frames of world**: off
        // the frame index rather than the clock, so the same frame stands under the same sky on
        // every machine. The crossing itself takes the same `sTurnSeconds`, which
        // `setTurnCrossings` gave the world's `Transition_Delta` before there was a world;
        // `MWWorld::WeatherManager` runs it and this does not touch it.
        mTurned += step / sTurnSeconds;
        if (mTurned < 1.0f)
            return;

        mTurned = 0.0f;
        mTurnedTo = (mTurnedTo + 1) % through.size();

        Stager::setWeather(*MWBase::Environment::get().getWorld(), through[mTurnedTo]);
    }

    void CameraDriver::aim(const Stop& stop)
    {
        if (stop.mSchedule.mFreeCamera || !stop.mStand.mEye.has_value())
            return;

        if (stop.mSchedule.mTrack.has_value())
        {
            aimCamera(mFlown, mFacing);
            return;
        }

        const std::optional<Route>& route = stop.mSchedule.mRoute;
        const osg::Vec3f look = route.has_value() ? route->mLookTo : mFlown + (mFromLook - mFrom);

        aimCamera(mFlown, Stand{ .mCell = {}, .mEye = mFlown, .mLook = look }.getRotation());
    }

    float CameraDriver::getTravelled(const Route& route) const
    {
        const float whole = (route.mTo - mFrom).length();
        return whole > 0.0f ? std::clamp((mFlown - mFrom).length() / whole, 0.0f, 1.0f) : 1.0f;
    }

    void CameraDriver::standAt(const osg::Vec3f& eye, const osg::Vec3f& look)
    {
        mFrom = eye;
        mFromLook = look;
        mFlown = eye;
    }

    void CameraDriver::standWhereThePlayerIs()
    {
        // The reference lives in the cell store rather than in the `Ptr`, which is what the named
        // player says: the position outlives the handle it was reached through.
        const MWWorld::Ptr player = MWBase::Environment::get().getWorld()->getPlayerPtr();
        const ESM::Position& stood = player.getRefData().getPosition();

        const osg::Vec3f eye(stood.pos[0], stood.pos[1], stood.pos[2]);
        standAt(eye, eye + Stand::forwardOf(osg::Vec3f(stood.rot[0], stood.rot[1], stood.rot[2])));
    }

    void CameraDriver::moveBodyTo(const osg::Vec3f& eye)
    {
        MWBase::World& world = *MWBase::Environment::get().getWorld();
        const MWWorld::Ptr player = world.getPlayerPtr();
        const ESM::Position& stood = player.getRefData().getPosition();

        // **`moveObjectBy` and not `moveObject`, because the player is an actor.** The actor's
        // position lives in the physics world as well, and a move that writes only the world's
        // copy is written back over it on the next step.
        world.moveObjectBy(player, eye - osg::Vec3f(stood.pos[0], stood.pos[1], stood.pos[2]), true);
    }

    void CameraDriver::aimCamera(const osg::Vec3f& eye, const osg::Vec3f& rotation)
    {
        MWRender::Camera* camera = MWBase::Environment::get().getWorld()->getRenderingManager()->getCamera();

        // **A static camera and not the player's own.** Nothing tracks the body, nothing rotates
        // to its facing and nothing casts a ray to keep the eye out of a wall, which is what a view
        // file's coordinates mean. It does not hold on its own, for the reason `aim` gives.
        camera->setMode(MWRender::Camera::Mode::Static);
        camera->setStaticPosition(osg::Vec3d(eye));

        // The body's rotation, negated into the camera's own angles the way
        // `Camera::rotateCameraToTrackingPtr` negates a tracked body's.
        camera->setPitch(-rotation.x(), true);
        camera->setYaw(-rotation.z(), true);
    }
}
