#include "session.hpp"

#include <algorithm>
#include <cmath>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <osg/Vec3d>

#include <components/debug/debuglog.hpp>
#include <components/esm/position.hpp>
#include <components/esm/refid.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtx/skylight.hpp>
#include <components/rtxbench/benchrecord.hpp>
#include <components/rtxbench/frametimes.hpp>
#include <components/rtxbench/gpuclock.hpp>
#include <components/rtxbench/perfcontrol.hpp>
#include <components/rtxbench/scenedigest.hpp>
#include <components/settings/values.hpp>

#include "../../mwbase/environment.hpp"
#include "../../mwbase/statemanager.hpp"
#include "../../mwbase/world.hpp"
#include "../../mwworld/cell.hpp"
#include "../../mwworld/cellstore.hpp"
#include "../../mwworld/datetimemanager.hpp"
#include "../../mwworld/globals.hpp"
#include "../../mwworld/ptr.hpp"
#include "../../mwworld/refdata.hpp"
#include "../../mwworld/timestamp.hpp"

#include "../camera.hpp"
#include "../renderingmanager.hpp"
#include "rtxrenderer.hpp"
#include "stopwriter.hpp"

namespace MWRender
{
    namespace
    {
        /// How often a run that turns its sky asks for the next weather, in frames of world.
        ///
        /// **How long the crossing itself takes is the weather's own `Transition_Delta`**, which
        /// `MWWorld::WeatherManager` runs and this does not touch. What is stated here is only the
        /// cadence of the asking, off the frame index rather than the clock, so the same frame
        /// stands under the same sky on every machine.
        constexpr float sTurnFrames = 4.0f * Rtx::sStepRate;

        /// How far ahead the `look` a run reports points.
        ///
        /// **A landmark's distance rather than a nose's.** The renderer wants a direction; a person
        /// reading `pos` and `look` in `views.cfg` wants to be able to tell where they point, and a
        /// cell is eight thousand units across.
        constexpr double sLookAhead = 1000.0;
    }

    std::optional<Rtx::SessionRequest> readSessionSetting()
    {
        const std::string spelling = Settings::rtx().mSession;
        if (spelling.empty())
            return std::nullopt;

        std::string complaint;
        const std::optional<Rtx::BenchSpec> spec = Rtx::readSpec(spelling, complaint);

        // **A run nobody can read the settings of is not a run.** Starting anyway would hand
        // somebody a number for a length they did not ask for.
        if (!spec.has_value())
        {
            Log(Debug::Error) << "[RTX] session: " << complaint;
            return std::nullopt;
        }

        Rtx::Stop stop;
        stop.mName = "the game";
        stop.mSchedule.mSpec = *spec;
        if (spec->mSpeed > 0.0f)
            stop.mSchedule.mRoute = Rtx::Route{ .mSpeed = spec->mSpeed };

        Rtx::SessionRequest request;
        request.mStops.push_back(std::move(stop));

        // **A window, because somebody asked for this in a game they can see.** The harness hides
        // its own; a settings file is read by the binary a player runs.
        request.mHeadless = false;
        request.mValidation.mEnabled = Rtx::sValidationByDefault;

        return request;
    }

    /// What a stop gathers, and the few things a whole run does. Out of line so the header names
    /// none of it.
    struct Session::Held
    {
        Rtx::FrameSamples mSamples;
        Rtx::GpuBreakdown mGpu;
        Rtx::Crossings mCrossings;
        Rtx::GpuClock mClock;

        /// perf's control fifo, held for the whole run so every stop brackets its own frames.
        std::unique_ptr<Rtx::PerfControl> mProfiling;

        StopWriter mWriter;

        /// What a hashed frame lands in, refilled per measured frame and never freed.
        ///
        /// **Not shared with the writer's**, which reads at a doll's or a tile's extent rather than
        /// the frame's — one buffer would grow to the largest of them and stay there.
        std::vector<std::uint8_t> mPixels;
    };

    Session::Session(Rtx::SessionRequest request, Rtx::SessionResult* const into)
        : mRequest(std::move(request))
        , mInto(into)
        , mHeld(std::make_unique<Held>())
    {
        mHeld->mProfiling = std::make_unique<Rtx::PerfControl>(mRequest.mPerfControl);

        if (!mRequest.mAgainst.empty())
            mRecord.readReference(mRequest.mAgainst);

        std::uint32_t longest = 0;
        for (const Rtx::Stop& stop : mRequest.mStops)
            longest = std::max(longest, stop.mSchedule.mSpec.getMeasured());

        // Reserved once at the longest stop's length, so no measured frame grows a vector — a
        // benchmark that stops to reallocate is measuring its own allocator.
        mHeld->mSamples.reserve(longest);

        mRecord.reserve(mRequest.mStops.size());

        if (mRequest.mStops.empty())
            mDone = true;
    }

    Session::~Session()
    {
        if (mInto != nullptr)
        {
            *mInto = describeRun();
            return;
        }

        // **Nobody installed this run, so nobody is waiting for what it came to.** A settings file
        // starts a session inside a played binary, where the log is the only reader there is — and
        // a report built and then dropped is a run nobody can read.
        if (const std::string& report = mRecord.getReport(); !report.empty())
            Log(Debug::Info) << "Ray tracing session:\n" << report;
    }

    void Session::noteStanding()
    {
        MWBase::World& world = *MWBase::Environment::get().getWorld();
        const Camera& camera = *world.getRenderingManager()->getCamera();
        const MWWorld::TimeStamp now = world.getTimeStamp();

        mStood = Note{
            .mAt = camera.getPosition(),
            .mFacing = camera.getOrient(),
            .mHour = now.getHour(),
            .mDay = now.getDay(),
            .mWeather = world.getCurrentWeatherScriptId(),
        };
    }

    Rtx::SessionResult Session::describeRun() const
    {
        Rtx::SessionResult result;
        result.mExitStatus = mRecord.getExitStatus();
        result.mPlaces.assign(mRecord.getPlaces().begin(), mRecord.getPlaces().end());
        result.mReport = mRecord.getReport();

        if (!mStood.has_value())
            return result;

        result.mLeft = Rtx::Standing{
            .mEye = osg::Vec3f(mStood->mAt),

            // The direction and not a point on it, for the reason `Rtx::makeCamera` gives — but a
            // view file holds a `look`, and a landmark's distance is what makes one readable.
            .mLook = osg::Vec3f(mStood->mAt + mStood->mFacing * osg::Vec3d(0.0, sLookAhead, 0.0)),
            .mHour = mStood->mHour,
            .mDay = mStood->mDay,
            .mWeather = std::string(Rtx::weatherName(static_cast<std::uint32_t>(mStood->mWeather))),
        };

        return result;
    }

    bool Session::isPlaying() const
    {
        if (MWBase::Environment::get().getStateManager()->getState() != MWBase::StateManager::State_Running)
            return false;

        MWBase::World* world = MWBase::Environment::get().getWorld();
        return world != nullptr && !world->getPlayerPtr().isEmpty();
    }

    void Session::aimCamera(const osg::Vec3f& eye, const osg::Vec3f& look)
    {
        Camera* camera = MWBase::Environment::get().getWorld()->getRenderingManager()->getCamera();

        osg::Vec3f along = look - eye;
        if (along.length2() <= 0.0f)
            along = osg::Vec3f(0.0f, 1.0f, 0.0f);
        along.normalize();

        // **A static camera and not the player's own.** Nothing tracks the body, nothing rotates
        // to its facing and nothing casts a ray to keep the eye out of a wall — which is what a
        // view file's coordinates mean, and what the built-in camera script leaves alone.
        camera->setMode(Camera::Mode::Static);
        camera->setStaticPosition(osg::Vec3d(eye));

        // **The engine's own basis, recovered rather than restated.** `Camera::getOrient` builds
        // the eye from a pitch about X and a yaw about Z and looks down +Y, so a camera facing the
        // actor's own heading is `setYaw(-rot[2])` — which makes the forward vector
        // `(-sin(yaw), cos(yaw), 0)` at level pitch, and this the inverse of it.
        camera->setPitch(std::asin(along.z()), true);
        camera->setYaw(std::atan2(-along.x(), along.y()), true);
    }

    void Session::standWhereThePlayerIs()
    {
        // The reference lives in the cell store rather than in the `Ptr`, which is what the named
        // player says: the position outlives the handle it was reached through.
        const MWWorld::Ptr player = MWBase::Environment::get().getWorld()->getPlayerPtr();
        const ESM::Position& stood = player.getRefData().getPosition();

        mFrom = osg::Vec3f(stood.pos[0], stood.pos[1], stood.pos[2]);
        mFromLook = mFrom + osg::Vec3f(std::sin(stood.rot[2]), std::cos(stood.rot[2]), 0.0f);
        mFlown = mFrom;
    }

    void Session::forgetHistory()
    {
        MWBase::Environment::get().getWorld()->getRenderingManager()->notifyWorldSpaceChanged();
    }

    void Session::beginStop()
    {
        const Rtx::Stop& stop = mRequest.mStops[mAt];
        MWBase::World& world = *MWBase::Environment::get().getWorld();

        // **The player goes first, because the ring is read around them and not around the eye.**
        // A camera placed in a cell nobody stands in is a camera looking at ground the simulation
        // never asked for.
        //
        // **The exterior is tried before the interior, which is the order `coc` tries them in.** A
        // pair of integers only resolves as an exterior, and a name can be either.
        if (!stop.mStand.mCell.empty())
        {
            ESM::Position where{};
            ESM::RefId found = world.findExteriorPosition(stop.mStand.mCell, where);
            if (found.empty())
                found = world.findInteriorPosition(stop.mStand.mCell, where);

            if (found.empty())
            {
                Log(Debug::Error) << "Ray tracing session: no cell is called \"" << stop.mStand.mCell << '"';
                mRecord.fail();
                mDone = true;
                MWBase::Environment::get().getStateManager()->requestQuit();
                return;
            }

            // **Where the eye goes and not where the cell centres, where the stop says.** The
            // position the world found is what stands a player in the cell; a view names the spot
            // its picture is of, and the ring is the same ring either way.
            if (stop.mStand.mEye.has_value())
            {
                where.pos[0] = stop.mStand.mEye->x();
                where.pos[1] = stop.mStand.mEye->y();
                where.pos[2] = stop.mStand.mEye->z();
            }

            world.changeToCell(found, where, true);
        }
        else if (stop.mStand.mEye.has_value())
            world.moveObject(world.getPlayerPtr(), *stop.mStand.mEye, true, true);

        // **Through the globals the console writes and not through the clock's own setters**, which
        // are `MWWorld::World`'s alone. `set gamehour to` and `set day to` are the same two calls,
        // so a stop stands at an hour and a date a player could have typed.
        if (stop.mSky.mHour.has_value())
            world.setGlobalFloat(MWWorld::Globals::sGameHour, *stop.mSky.mHour);

        if (stop.mSky.mDay.has_value())
            world.setGlobalInt(MWWorld::Globals::sDay, *stop.mSky.mDay);

        if (stop.mSky.mWeather.has_value())
        {
            const std::optional<std::uint32_t> named = Rtx::weatherIndex(*stop.mSky.mWeather);
            if (named.has_value())
                world.changeWeather(world.getPlayerPtr().getCell()->getCell()->getRegion(), *named);
            else
                Log(Debug::Warning) << "Ray tracing session: no weather is called \"" << *stop.mSky.mWeather << '"';
        }

        // **Settled rather than crossed into**, which is what the game does when a player sleeps:
        // a stop asked to stand under a sky stands under it from its first frame rather than four
        // seconds later. A run that turns its sky asks for the transition instead.
        if (stop.mSky.mHour.has_value() || stop.mSky.mDay.has_value() || stop.mSky.mWeather.has_value())
            world.advanceTime(0.0, false);

        if (!stop.mSky.mTurnThrough.empty())
        {
            const std::optional<std::uint32_t> first = Rtx::weatherIndex(stop.mSky.mTurnThrough.front());
            if (first.has_value())
                world.changeWeather(world.getPlayerPtr().getCell()->getCell()->getRegion(), *first);
        }

        // **The clock stops after the world has been moved and not before.** A frozen stop is a
        // reference: nothing animates, so a frame traced many times is the same frame and an
        // accumulated picture converges on the integral rather than on the animation.
        world.getTimeManager()->setSimulationTimeScale(stop.mSchedule.mFrozen ? 0.0f : 1.0f);

        const MWWorld::Ptr player = world.getPlayerPtr();
        mCell = player.getCell();

        if (stop.mSchedule.mFreeCamera)
        {
            // **The walls come off, because a view file names where a camera stands.** Half of them
            // are inside a rock or over the sea, and a body dropped there either falls or cannot be
            // put there at all.
            //
            // **Toggled until it is off, because the call reports rather than sets.** `tcl` is the
            // same call, and a session that had already used it would otherwise turn collision back
            // on.
            if (world.toggleCollisionMode())
                world.toggleCollisionMode();

            // **Read after that toggle and not before.** Turning collision on calls
            // `World::adjustPosition`, which drops the player onto the ground — so a camera placed
            // from a position read above this stands where nobody ended up.
            standWhereThePlayerIs();
        }
        else if (stop.mStand.mEye.has_value())
        {
            mFrom = *stop.mStand.mEye;
            mFromLook = stop.mStand.mLook.value_or(mFrom + osg::Vec3f(0.0f, 1.0f, 0.0f));
            mFlown = mFrom;
            aimCamera(mFrom, mFromLook);
        }
        else
        {
            standWhereThePlayerIs();
        }

        // **A stop is a discontinuity, and only a worldspace change says so on its own.** A
        // teleport from Balmora to Vivec stays in one worldspace, so nothing tells the renderer its
        // history describes somewhere else — and the exposure adapts toward its measurement over
        // seconds rather than taking it, so a room drawn after a noon exterior opens at the
        // exterior's brightness. The warm-up absorbs the frame it costs.
        forgetHistory();

        mSeen = 0;
        mTurnedTo = 0;
        mTurned = 0.0f;
        mHeld->mSamples.clear();
        mHeld->mGpu = Rtx::GpuBreakdown{};
        mHeld->mCrossings = Rtx::Crossings{};
        mHeld->mClock = Rtx::GpuClock{};
        mHitPercent = 0.0;
        mWallMs = 0.0;
        mStarted = true;

        Log(Debug::Info) << "Ray tracing session: stop " << (mAt + 1) << " of " << mRequest.mStops.size() << ", "
                         << (stop.mName.empty() ? "unnamed" : stop.mName) << " — " << stop.mSchedule.mSpec.getWarmup()
                         << " frames warming up then " << stop.mSchedule.mSpec.getMeasured() << " measured";
    }

    void Session::fly()
    {
        const Rtx::Stop& stop = mRequest.mStops[mAt];
        if (!stop.mSchedule.mRoute.has_value())
            return;

        const Rtx::Route& route = *stop.mSchedule.mRoute;
        if (!(route.mSpeed > 0.0f))
            return;

        MWBase::World& world = *MWBase::Environment::get().getWorld();
        const MWWorld::Ptr player = world.getPlayerPtr();
        if (player.isEmpty())
            return;

        const ESM::Position& stood = player.getRefData().getPosition();
        const osg::Vec3f standing(stood.pos[0], stood.pos[1], stood.pos[2]);

        // **The heading the engine measures**, which is clockwise from north rather than
        // counter-clockwise from east, and horizontal: a route follows the ground the cells are
        // laid out on, and the pitch a save happens to have left would fly it into the sky.
        //
        // **Measured from `mFlown` and never from the player**, which says why.
        osg::Vec3f along = route.mTo.has_value() ? *route.mTo - mFlown
                                                 : osg::Vec3f(std::sin(stood.rot[2]), std::cos(stood.rot[2]), 0.0f);

        const float left = along.length();
        if (route.mTo.has_value() && left <= 0.0f)
            return;

        along.normalize();

        // **Off the frame index and not the clock**, for the reason the world is stepped that way:
        // a camera advanced by how long the last frame took crosses its boundaries somewhere else
        // on every machine, and where they fall is the whole measurement.
        float step = route.mSpeed * Rtx::sStepSeconds;
        if (route.mTo.has_value())
            step = std::min(step, left);

        // **The height needs no correction of its own.** A route with no destination has a heading
        // flat in z, so it keeps the height it began at. One with a destination takes its height
        // from the line between the two ends, which is what a view states when it names both.
        mFlown += along * step;

        // **`moveObjectBy` and not `moveObject`, because the player is an actor.** The actor's
        // position lives in the physics world as well, and a move that writes only the world's
        // copy is written back over it on the next step.
        world.moveObjectBy(player, mFlown - standing, true);

        if (stop.mStand.mEye.has_value())
        {
            const osg::Vec3f look = route.mLookTo.has_value() ? *route.mLookTo : mFlown + (mFromLook - mFrom);
            aimCamera(mFlown, look);
        }
    }

    void Session::turnWeather()
    {
        const std::vector<std::string>& through = mRequest.mStops[mAt].mSky.mTurnThrough;
        if (through.size() < 2)
            return;

        mTurned += 1.0f / sTurnFrames;
        if (mTurned < 1.0f)
            return;

        mTurned = 0.0f;
        mTurnedTo = (mTurnedTo + 1) % through.size();

        MWBase::World& world = *MWBase::Environment::get().getWorld();
        const std::optional<std::uint32_t> named = Rtx::weatherIndex(through[mTurnedTo]);
        if (named.has_value())
            world.changeWeather(world.getPlayerPtr().getCell()->getCell()->getRegion(), *named);
    }

    std::optional<std::uint32_t> Session::getSampleFrame() const
    {
        if (mDone || !mStarted)
            return std::nullopt;

        return mSeen;
    }

    std::uint32_t Session::getAccumulated() const
    {
        if (mDone || !mStarted)
            return 0;

        const Rtx::Stop& stop = mRequest.mStops[mAt];
        const std::uint32_t warmup = stop.mSchedule.mSpec.getWarmup();
        if (stop.mSchedule.mAccumulate == 0 || mSeen < warmup)
            return 0;

        // **Counted from the first measured frame**, because the warm-up is the world arriving and
        // the card coming off its idle clock. Averaging those in would put a picture of a
        // half-built cell into the reference.
        return mSeen - warmup + 1;
    }

    void Session::beforeFrame()
    {
        if (mDone || !isPlaying())
            return;

        if (!mStarted)
        {
            beginStop();
            return;
        }

        // **The reset stands until a frame has been counted, and the one `beginStop` issued is not
        // enough.** A stop opens with frames nobody counts: the first trace after the teleport has
        // no predecessor to be timed against, so `frame` is never reached for it and `mSeen` stays
        // at nought. The exposure adapts on every one of them all the same, and how many there are
        // is a question about how long the world took to load rather than one the schedule answers
        // — so two runs began counting from two exposures and drew the first thirty frames
        // differently, with the same scene behind them. Reissued here, the last reset lands on the
        // frame that becomes the first counted one, whatever went before it.
        //
        // **Both calls, and neither is the other's spare.** `beginStop` resets because a teleport
        // is a discontinuity and the frames it opens with are drawn on a screen. This resets
        // because those frames are not measured, and a measured run may not depend on them.
        if (mSeen == 0)
            forgetHistory();

        // **The route runs over the measured frames and not the warm-up.** Warming up is the GPU
        // coming off its idle clock; flying during it would start the measurement partway along
        // and leave the first crossing outside the numbers.
        if (mSeen >= mRequest.mStops[mAt].mSchedule.mSpec.getWarmup())
        {
            fly();
            turnWeather();
        }

        // **After the schedule has moved, because the note is of the frame about to be drawn.** The
        // route flies the eye and the turn crosses the sky above it, both between this call and the
        // trace — so a note taken before them describes a camera under a sky that no frame ever
        // used. The last one taken is what `describeRun` publishes.
        noteStanding();
    }

    bool Session::wantsSecondWalk() const
    {
        return !mDone && mStarted && mRequest.mStops[mAt].mActions.mWalkTwice;
    }

    void Session::frame(const TracedRun& run, const Rtx::FrameResult& result, const double frameMs, const double walkMs,
        const double placeMs, const double warmMs, const bool rebuilt)
    {
        Rtx::Renderer& renderer = run.mBackend;

        if (mDone || !mStarted)
            return;

        const Rtx::Stop& stop = mRequest.mStops[mAt];
        const std::uint32_t warmup = stop.mSchedule.mSpec.getWarmup();
        const std::uint32_t measured = stop.mSchedule.mSpec.getMeasured();

        if (mSeen == warmup)
        {
            // **Where the measured frames begin, and again where they end.** One reading is one
            // moment: taken only at the end it is a card already climbing off the load, and it
            // would print a fast clock over frames drawn at a slower one.
            mHeld->mClock.add(Rtx::readGpuClock());
            mHeld->mProfiling->enable();
        }

        ++mSeen;

        if (mSeen <= warmup)
            return;

        mHeld->mSamples.add(frameMs, walkMs, placeMs, warmMs);
        mHeld->mSamples.addWait(result.mWaitMs);
        mHeld->mGpu.add(result.mGpu);
        mWallMs += frameMs;

        // **Counted here and not where the route moved**, because a crossing is a dropped frame and
        // this is where what it dropped is known. The move pulls the next ring in and that read
        // lands in the frame after it — which is the frame that arrives here standing in a cell it
        // was not drawn in last time, and the frame that paid for it.
        //
        // **The whole frame goes in as the read**, because the game gives no split: the ring
        // arrives on the loading threads, and what a crossing costs here is the frame that dropped.
        if (const void* cell = MWBase::Environment::get().getWorld()->getPlayerPtr().getCell();
            mCell != nullptr && cell != mCell)
        {
            mHeld->mCrossings.add(rebuilt, frameMs, 0.0);
            mCell = cell;
        }

        const Rtx::FrameExtents extents = renderer.getExtents();
        const double traced = static_cast<double>(extents.mRenderWidth) * extents.mRenderHeight;
        if (traced > 0.0)
            mHitPercent = static_cast<double>(result.mHits) / traced * 100.0;

        const std::uint32_t drawn = mSeen - warmup;

        if (stop.mActions.mHash)
        {
            renderer.readPixels(mHeld->mPixels);

            mRecord.getHashes().add(stop.mName, drawn, mHeld->mPixels, Rtx::digestParts(run.mScene.getTables()));
        }

        if (drawn < measured)
            return;

        endStop(run, result.mReconstruction);
    }

    void Session::endStop(const TracedRun& run, const Rtx::Reconstruction& reconstruction)
    {
        const Rtx::Stop& stop = mRequest.mStops[mAt];
        Rtx::Renderer& renderer = run.mBackend;

        mHeld->mProfiling->disable();

        // After the frames and not before them, so the process spawn it costs is outside the run
        // it describes.
        mHeld->mClock.add(Rtx::readGpuClock());

        const Rtx::FrameExtents extents = renderer.getExtents();

        if (mRecord.empty())
        {
            // **Taken at the first stop, because every stop of a run is traced by one renderer.**
            // What the record's header states is the configuration the whole run stood under, and
            // asking the renderer is the only way to know what the upscaler settled on.
            Rtx::BenchHeader& header = mRecord.getHeader();
            header.mExtents = extents;
            header.mUpscale = renderer.getUpscale();
            header.mValidating = renderer.isValidating();
            header.mMeasured = stop.mSchedule.mSpec.getMeasured();
            header.mWarmup = stop.mSchedule.mSpec.getWarmup();
        }

        mHeld->mWriter.write(run, reconstruction, stop.mActions, mHeld->mCrossings, mRecord);

        Rtx::BenchPlace place;
        place.mView = stop.mName;
        place.mCell = stop.mStand.mCell;
        place.mNote = stop.mNote;
        place.mHour = MWBase::Environment::get().getWorld()->getTimeStamp().getHour();
        place.mWeather = stop.mSky.mWeather.value_or(std::string());
        place.mFrames = mHeld->mSamples.size();
        place.mWallSeconds = mWallMs / 1000.0;
        for (std::size_t at = 0; at < Rtx::sTimingCount; ++at)
            place.mRows[at] = Rtx::summarise(mHeld->mSamples.mRows[at]);
        place.mClock = mHeld->mClock;
        place.mHitPercent = mHitPercent;
        place.mCrossings = mHeld->mCrossings;
        place.mScene = renderer.getSceneStats();
        place.mMemory = renderer.getMemoryReport();

        const std::span<const Rtx::GpuZone> zones = mHeld->mGpu.summariseZones();
        place.mGpu.assign(zones.begin(), zones.end());

        mRecord.add(std::move(place));

        mStarted = false;
        ++mAt;

        if (mAt < mRequest.mStops.size())
            return;

        finish();
    }

    void Session::finish()
    {
        mDone = true;
        mRecord.finish(mRequest);

        // **The way the quit key ends a session, and not `exit`.** A run that tore the process down
        // where it stood would leave the save, the log and the device wherever they happened to be,
        // and the next thing anyone would debug is the session.
        if (mRequest.mQuitAtEnd)
            MWBase::Environment::get().getStateManager()->requestQuit();
    }
}
