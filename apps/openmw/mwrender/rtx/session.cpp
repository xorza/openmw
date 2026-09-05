#include "session.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <format>
#include <fstream>
#include <span>
#include <utility>
#include <vector>

#include <osg/Vec3d>

#include <components/debug/debuglog.hpp>
#include <components/esm/position.hpp>
#include <components/esm/refid.hpp>
#include <components/files/conversion.hpp>
#include <components/misc/constants.hpp>
#include <components/myguirtx/texture.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/rtx/frametimes.hpp>
#include <components/rtx/lightbuilder.hpp>
#include <components/rtx/png.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/sceneextractor.hpp>
#include <components/rtx/shaders/colour.h>
#include <components/rtx/texturebuilder.hpp>
#include <components/rtxbench/contactsheet.hpp>
#include <components/rtxbench/framehashes.hpp>
#include <components/rtxbench/gpuclock.hpp>
#include <components/rtxbench/perfcontrol.hpp>
#include <components/rtxbench/scenedigest.hpp>
#include <components/sceneutil/offscreenframing.hpp>
#include <components/settings/values.hpp>

#include "../../mwbase/environment.hpp"
#include "../../mwbase/statemanager.hpp"
#include "../../mwbase/world.hpp"
#include "../../mwworld/cell.hpp"
#include "../../mwworld/cellstore.hpp"
#include "../../mwworld/datetimemanager.hpp"
#include "../../mwworld/esmstore.hpp"
#include "../../mwworld/globals.hpp"
#include "../../mwworld/manualref.hpp"
#include "../../mwworld/ptr.hpp"
#include "../../mwworld/refdata.hpp"
#include "../../mwworld/timestamp.hpp"

#include "../camera.hpp"
#include "../characterpreview.hpp"
#include "../offscreenview.hpp"
#include "../renderingmanager.hpp"
#include "rtxrenderer.hpp"

namespace MWRender
{
    namespace
    {
        /// The run this process was asked to make, and what it came to.
        ///
        /// **Two slots rather than a channel through `RendererSpec`.** That struct is filled inside
        /// `Engine::go`, and a field there would be an edit to upstream for a value one launcher
        /// sets and every other caller leaves empty.
        std::optional<SessionRequest> sInstalled;
        SessionResult sResult;

        /// How often a run that turns its sky asks for the next weather, in frames of world.
        ///
        /// **How long the crossing itself takes is the weather's own `Transition_Delta`**, which
        /// `MWWorld::WeatherManager` runs and this does not touch. What is stated here is only the
        /// cadence of the asking, off the frame index rather than the clock, so the same frame
        /// stands under the same sky on every machine.
        constexpr float sTurnFrames = 4.0f * Rtx::sStepRate;

        /// How wide a map tile is written. The game's own resolution, because the size is not what
        /// the picture is for.
        constexpr int sMapTileSide = 512;

        /// Where the tile's eye stands and how far it sees, both far enough to clear any cell.
        constexpr float sMapEyeHeight = 50000.0f;
        constexpr float sMapFar = 150000.0f;

        /// How far ahead the `look` a run reports points.
        ///
        /// **A landmark's distance rather than a nose's.** The renderer wants a direction; a person
        /// reading `pos` and `look` in `views.cfg` wants to be able to tell where they point, and a
        /// cell is eight thousand units across.
        constexpr double sLookAhead = 1000.0;
    }

    std::optional<SessionRequest> readSessionSetting()
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

        Stop stop;
        stop.mName = "the game";
        stop.mSchedule.mSpec = *spec;
        if (spec->mSpeed > 0.0f)
            stop.mSchedule.mRoute = Route{ .mSpeed = spec->mSpeed };

        SessionRequest request;
        request.mStops.push_back(std::move(stop));

        // **A window, because somebody asked for this in a game they can see.** The harness hides
        // its own; a settings file is read by the binary a player runs.
        request.mHeadless = false;
        request.mValidation.mEnabled = Rtx::sValidationByDefault;

        return request;
    }

    void installSession(SessionRequest request)
    {
        sInstalled = std::move(request);
    }

    std::optional<SessionRequest> takeInstalledSession()
    {
        return std::exchange(sInstalled, std::nullopt);
    }

    void publishSessionResult(SessionResult result)
    {
        sResult = std::move(result);
    }

    /// **The whole slot, and never a field at a time.** A launcher reads eight of these and a run
    /// fills all eight, so a take written out member by member loses whichever ones nobody
    /// remembered — silently, since an unfilled `SessionResult` is a valid one describing a camera
    /// at the origin.
    SessionResult takeSessionResult()
    {
        return std::exchange(sResult, SessionResult{});
    }

    /// What a stop gathers, and the few things a whole run does. Out of line so the header names
    /// none of it.
    struct Session::Held
    {
        Rtx::FrameSamples mSamples;
        Rtx::GpuBreakdown mGpu;
        Rtx::Crossings mCrossings;
        Rtx::GpuClock mClock;

        Rtx::FrameHashes mHashes;
        Rtx::FrameHashes mReference;

        /// The last measured frame's own hash, and whether the frame before it hashed the same.
        ///
        /// **What says a still camera resolved to a still picture.** Two frames of one scene from
        /// one eye differ only if something carried state it should not have, and there is nothing
        /// else in this fork that can see that.
        std::array<std::uint64_t, 2> mLastHash{};
        bool mHadHash = false;
        bool mSettled = false;

        /// perf's control fifo, held for the whole run so every stop brackets its own frames.
        std::unique_ptr<Rtx::PerfControl> mProfiling;

        /// What a read back lands in, refilled per frame and never freed.
        std::vector<std::uint8_t> mPixels;
    };

    Session::Session(SessionRequest request)
        : mRequest(std::move(request))
        , mHeld(std::make_unique<Held>())
    {
        mHeld->mProfiling = std::make_unique<Rtx::PerfControl>(mRequest.mPerfControl);

        if (!mRequest.mAgainst.empty())
            mHeld->mReference = Rtx::FrameHashes::read(mRequest.mAgainst);

        std::uint32_t longest = 0;
        for (const Stop& stop : mRequest.mStops)
            longest = std::max(longest, stop.mSchedule.mSpec.getMeasured());

        // Reserved once at the longest stop's length, so no measured frame grows a vector — a
        // benchmark that stops to reallocate is measuring its own allocator.
        mHeld->mSamples.reserve(longest);

        mPlaces.reserve(mRequest.mStops.size());

        if (mRequest.mStops.empty())
            mDone = true;
    }

    Session::~Session()
    {
        publishSessionResult(describeRun());
    }

    void Session::noteStanding()
    {
        MWBase::World& world = *MWBase::Environment::get().getWorld();
        const Camera& camera = *world.getRenderingManager()->getCamera();
        const MWWorld::TimeStamp now = world.getTimeStamp();

        mStood = Standing{
            .mAt = camera.getPosition(),
            .mFacing = camera.getOrient(),
            .mHour = now.getHour(),
            .mDay = now.getDay(),
            .mWeather = world.getCurrentWeatherScriptId(),
        };
    }

    SessionResult Session::describeRun() const
    {
        SessionResult result;
        result.mExitStatus = mExitStatus;
        result.mPlaces = mPlaces;
        result.mReport = mReport;

        if (!mStood.has_value())
            return result;

        result.mEye = osg::Vec3f(mStood->mAt);

        // The direction and not a point on it, for the reason `Rtx::makeCamera` gives — but a view
        // file holds a `look`, and a landmark's distance is what makes one readable.
        result.mLook = osg::Vec3f(mStood->mAt + mStood->mFacing * osg::Vec3d(0.0, sLookAhead, 0.0));

        result.mHour = mStood->mHour;
        result.mDay = mStood->mDay;
        result.mWeather = Rtx::weatherName(static_cast<std::uint32_t>(mStood->mWeather));

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

    void Session::beginStop()
    {
        const Stop& stop = mRequest.mStops[mAt];
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
                mExitStatus = 1;
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

            // The reference lives in the cell store rather than in the `Ptr`, which is what the
            // named player says: the position outlives the handle it was reached through.
            const MWWorld::Ptr player = world.getPlayerPtr();
            const ESM::Position& stood = player.getRefData().getPosition();
            mFrom = osg::Vec3f(stood.pos[0], stood.pos[1], stood.pos[2]);
            mFromLook = mFrom + osg::Vec3f(std::sin(stood.rot[2]), std::cos(stood.rot[2]), 0.0f);
        }
        else if (stop.mStand.mEye.has_value())
        {
            mFrom = *stop.mStand.mEye;
            mFromLook = stop.mStand.mLook.value_or(mFrom + osg::Vec3f(0.0f, 1.0f, 0.0f));
            aimCamera(mFrom, mFromLook);
        }
        else
        {
            const ESM::Position& stood = player.getRefData().getPosition();
            mFrom = osg::Vec3f(stood.pos[0], stood.pos[1], stood.pos[2]);
            mFromLook = mFrom + osg::Vec3f(std::sin(stood.rot[2]), std::cos(stood.rot[2]), 0.0f);
        }

        // **A stop is a discontinuity, and only a worldspace change says so on its own.** A
        // teleport from Balmora to Vivec stays in one worldspace, so nothing tells the renderer its
        // history describes somewhere else — and the exposure adapts toward its measurement over
        // seconds rather than taking it, so a room drawn after a noon exterior opens at the
        // exterior's brightness. The warm-up absorbs the frame it costs.
        MWBase::Environment::get().getWorld()->getRenderingManager()->notifyWorldSpaceChanged();

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
        const Stop& stop = mRequest.mStops[mAt];
        if (!stop.mSchedule.mRoute.has_value())
            return;

        const Route& route = *stop.mSchedule.mRoute;
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
        osg::Vec3f along = route.mTo.has_value() ? *route.mTo - standing
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

        osg::Vec3f moved = along * step;

        // **Held at the height the stop began at**, because nothing here flies: gravity would sink
        // a route into the sea over ten seconds, and a route that ends underwater measures the
        // wrong frame. The ground still rises through it, so a stretch of a long route is inside a
        // hill.
        if (!route.mTo.has_value())
            moved.z() = mFrom.z() - standing.z();

        // **`moveObjectBy` and not `moveObject`, because the player is an actor.** The actor's
        // position lives in the physics world as well, and a move that writes only the world's
        // copy is written back over it on the next step.
        world.moveObjectBy(player, moved, true);

        if (stop.mStand.mEye.has_value())
        {
            const osg::Vec3f eye = standing + moved;
            const osg::Vec3f look = route.mLookTo.has_value() ? *route.mLookTo : eye + (mFromLook - mFrom);
            aimCamera(eye, look);
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

        const Stop& stop = mRequest.mStops[mAt];
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

    void Session::frame(RtxRenderer& owner, const Rtx::FrameResult& result, const double frameMs, const double walkMs,
        const double placeMs, const bool rebuilt)
    {
        Rtx::Renderer& renderer = owner.getBackend();

        if (mDone || !mStarted)
            return;

        const Stop& stop = mRequest.mStops[mAt];
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

        mHeld->mSamples.add(frameMs, walkMs, placeMs);
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

        const bool watching
            = std::find(stop.mActions.mChecks.begin(), stop.mActions.mChecks.end(), Check::PictureSettles)
            != stop.mActions.mChecks.end();

        if (stop.mActions.mHash || watching)
        {
            renderer.readPixels(mHeld->mPixels);

            if (stop.mActions.mHash)
                mHeld->mHashes.add(stop.mName, drawn, mHeld->mPixels);

            Rtx::Digest digest;
            digest.add(std::span<const std::uint8_t>(mHeld->mPixels));

            mHeld->mSettled = mHeld->mHadHash && digest.getWords() == mHeld->mLastHash;
            mHeld->mLastHash = digest.getWords();
            mHeld->mHadHash = true;
        }

        if (drawn < measured)
            return;

        endStop(owner);
    }

    void Session::endStop(RtxRenderer& owner)
    {
        const Stop& stop = mRequest.mStops[mAt];
        Rtx::Renderer& renderer = owner.getBackend();

        mHeld->mProfiling->disable();

        // After the frames and not before them, so the process spawn it costs is outside the run
        // it describes.
        mHeld->mClock.add(Rtx::readGpuClock());

        const Rtx::FrameExtents extents = renderer.getExtents();

        // **The last measured frame, which is the one every figure above describes.** A picture
        // taken from any other frame is a picture of a run this report is not about.
        if (!stop.mActions.mCapture.empty())
        {
            renderer.readPixels(mHeld->mPixels);
            try
            {
                Rtx::writePng(stop.mActions.mCapture, extents.mOutputWidth, extents.mOutputHeight, mHeld->mPixels);
                mReport += std::format("wrote {} {}x{}", Files::pathToUnicodeString(stop.mActions.mCapture),
                    extents.mOutputWidth, extents.mOutputHeight);

                if (extents.mRenderWidth != extents.mOutputWidth || extents.mRenderHeight != extents.mOutputHeight)
                    mReport += std::format(", traced at {}x{}", extents.mRenderWidth, extents.mRenderHeight);

                mReport += '\n';
            }
            catch (const std::exception& failed)
            {
                mReport += std::format(
                    "could not write {}: {}\n", Files::pathToUnicodeString(stop.mActions.mCapture), failed.what());
                mExitStatus = 1;
            }
        }

        // **The bounce's tail, in radiance and not in bytes.** A firefly is a bounce far enough
        // above what the pixel has been seeing to be an outlier, and that is a statement about
        // scene-referred light: the display curve has spent the range it lives in long before a
        // pixel is a byte. Read off the channel the accumulator wrote, so what is counted is what
        // the clamp has already been over.
        if (stop.mActions.mTail)
        {
            std::vector<float> bounce;
            renderer.readChannel(Rtx::Channel::Accumulated, bounce);

            // The ladder the fork's own table was taken on. One is about where the signal ends — a
            // surface seeing a full hemisphere of sky — and everything past it is the tail proper.
            static constexpr std::array<float, 5> sThresholds{ 0.5f, 1.0f, 8.0f, 32.0f, 64.0f };
            std::array<std::uint64_t, 5> over{};

            const std::size_t counted = bounce.size() / 4;
            for (std::size_t at = 0; at < counted; ++at)
            {
                // **The renderer's own weights and not a copy of them.** A second set would be a
                // second idea of which of two things is brighter, and this is what decides which of
                // a frame's pixels are outliers.
                const float lit = bounce[at * 4] * Rtx::Shaders::LUMINANCE_WEIGHTS.x()
                    + bounce[at * 4 + 1] * Rtx::Shaders::LUMINANCE_WEIGHTS.y()
                    + bounce[at * 4 + 2] * Rtx::Shaders::LUMINANCE_WEIGHTS.z();

                for (std::size_t step = 0; step < sThresholds.size(); ++step)
                    if (lit > sThresholds[step])
                        ++over[step];
            }

            mReport += "bounce tail:";
            for (std::size_t step = 0; step < sThresholds.size(); ++step)
                mReport += std::format("{}>{} {:.4f}%", step == 0 ? " " : ", ", sThresholds[step],
                    counted > 0 ? static_cast<double>(over[step]) / static_cast<double>(counted) * 100.0 : 0.0);

            mReport += '\n';
        }

        // **The frame a measurement is taken on**, which is not the frame a picture is looked at.
        // Raw floats and no container: what reads this is a script computing an error against
        // another one, and every image format that carries floats would have to be decoded first.
        if (!stop.mActions.mDump.empty())
        {
            std::vector<float> radiance;
            renderer.readChannel(Rtx::Channel::Radiance, radiance);

            std::ofstream file(stop.mActions.mDump, std::ios::binary);
            file.write(reinterpret_cast<const char*>(radiance.data()),
                static_cast<std::streamsize>(radiance.size() * sizeof(float)));

            if (!file)
            {
                mReport += "could not write " + Files::pathToUnicodeString(stop.mActions.mDump) + '\n';
                mExitStatus = 1;
            }
        }

        if (mPlaces.empty())
        {
            // **Taken at the first stop, because every stop of a run is traced by one renderer.**
            // What the record's header states is the configuration the whole run stood under, and
            // asking the renderer is the only way to know what the upscaler settled on.
            mHeader.mExtents = extents;
            mHeader.mUpscale = renderer.getUpscale();
            mHeader.mValidating = renderer.isValidating();
            mHeader.mMeasured = stop.mSchedule.mSpec.getMeasured();
            mHeader.mWarmup = stop.mSchedule.mSpec.getWarmup();
        }

        if (stop.mActions.mDigest)
            reportScene(owner);

        if (!stop.mActions.mSheet.empty())
            writeSheet(owner, stop.mActions.mSheet);

        if (!stop.mActions.mMapTile.empty())
            writeMapTile(owner, stop.mActions.mMapTile);

        if (!stop.mActions.mDoll.empty())
            writeDoll(owner, stop.mActions.mDoll, stop.mActions.mDollOut);

        if (!stop.mActions.mFind.empty())
            reportFound(owner, stop.mActions.mFind);

        if (!stop.mActions.mChecks.empty())
            runChecks(owner);

        Rtx::BenchPlace place;
        place.mView = stop.mName;
        place.mCell = stop.mCell;
        place.mNote = stop.mNote;
        place.mHour = MWBase::Environment::get().getWorld()->getTimeStamp().getHour();
        place.mWeather = stop.mSky.mWeather.value_or(std::string());
        place.mFrames = mHeld->mSamples.size();
        place.mWallSeconds = mWallMs / 1000.0;
        place.mFrame = Rtx::summarise(mHeld->mSamples.mFrame);
        place.mWait = Rtx::summarise(mHeld->mSamples.mWait);
        place.mWalk = Rtx::summarise(mHeld->mSamples.mWalk);
        place.mPlace = Rtx::summarise(mHeld->mSamples.mPlace);
        place.mClock = mHeld->mClock;
        place.mHitPercent = mHitPercent;
        place.mCrossings = mHeld->mCrossings;
        place.mScene = renderer.getSceneStats();

        const std::span<const Rtx::GpuZone> zones = mHeld->mGpu.summariseZones();
        place.mGpu.assign(zones.begin(), zones.end());

        mPlaces.push_back(std::move(place));
        mReport += Rtx::describePlace(mPlaces.back());

        mStarted = false;
        ++mAt;

        if (mAt < mRequest.mStops.size())
            return;

        finish();
    }

    void Session::reportScene(RtxRenderer& owner)
    {
        const Rtx::SceneDesc& scene = owner.getMirror().getScene();
        const Rtx::ExtractionStats& stats = owner.getWalkStats();

        mReport += std::format(
            "\nplaced\n"
            "  instances:            {}\n"
            "  meshes:               {}\n"
            "  materials:            {}\n"
            "  textures:             {}\n"
            "  triangles:            {}\n"
            "  vertex+index bytes:   {} KiB\n"
            "  handed over:          {}\n",
            scene.getPlacedCount(), scene.getMeshes().size(), scene.getMaterials().size(), scene.getTextures().size(),
            scene.getTriangleCount(), scene.getGeometryBytes() / 1024, Rtx::digestScene(scene));

        for (std::size_t at = 0; at < stats.mTextureFormats.size(); ++at)
        {
            const Rtx::FormatCount& count = stats.mTextureFormats[at];
            const auto format = static_cast<Rtx::ImageFormat>(at);

            if (count.mMipped > 0)
                mReport += std::format("  {} x {}, with mips\n", count.mMipped, Rtx::nameOf(format));
            if (count.mMet > count.mMipped)
                mReport += std::format("  {} x {}, one level\n", count.mMet - count.mMipped, Rtx::nameOf(format));
            if (count.mMet > 0 && format == Rtx::ImageFormat::Unnamed)
                mReport += std::format("    which was pixel format {}\n", stats.mUnnamedFormat);
        }

        // Which materials traversal will have to stop and ask about, which of those asked for it
        // outright, and which of them a cutoff cannot answer for at all. The second and third being
        // the small ones is the point: Morrowind keeps its foliage under `NiAlphaProperty` rather
        // than under an alpha test, and almost nothing it ships is translucent in its own right.
        //
        // **Counted off the scene and not off a walk's own account.** What a walk reports it met is
        // what *that* walk met, and a chunk flattened once is nought in every walk after it. The
        // scene carries both facts per row.
        std::uint32_t cutouts = 0;
        std::uint32_t tested = 0;
        std::uint32_t translucent = 0;
        std::uint32_t media = 0;
        std::uint32_t glowing = 0;
        std::uint32_t flattened = 0;
        for (const Rtx::Material& material : scene.getMaterials())
        {
            cutouts += material.isCutout() ? 1 : 0;
            tested += material.mAlphaMode == Rtx::AlphaMode::Cutout ? 1 : 0;
            translucent += material.isTranslucent() ? 1 : 0;
            media += material.isMedium() ? 1 : 0;
            glowing += material.mEmissiveColour.length2() > 0.0f || material.mEmissive != Rtx::sNoIndex ? 1 : 0;
            flattened += material.mFlatten ? 1 : 0;
        }

        std::uint32_t sheets = 0;
        for (const Rtx::MeshRange& mesh : scene.getMeshes())
            sheets += mesh.mShape.mSheet ? 1 : 0;

        mReport += std::format(
            "  cutout materials:     {}, {} of them alpha-tested outright\n"
            "  translucent:          {}, which a cutoff cannot answer for\n"
            "  media:                {} of those are nowhere opaque\n"
            "  emissive materials:   {}\n"
            "  lights:               {} casting\n"
            "  deforming drawables:  {}\n"
            "  unbakeable cutouts:   {} placements of a mask a controller moves\n"
            "  flattened ground:     {} chunks past a cell\n"
            "  emitters:             {} holding {} live particles\n",
            cutouts, tested, translucent, media, glowing, scene.getLights().size(), stats.mDeformed, stats.mUnbakeable,
            flattened, stats.mEmitters, stats.mSprites);

        mReport += std::format(
            "\nnot placed\n"
            "  unreadable drawables: {}\n"
            "  unskinned rigs:       {} met before an update found their skeleton\n"
            "  empty geometry:       {}\n"
            "  undescribed surfaces: {}\n"
            "  worn otherwise:       {} placements wearing another material than their mesh\n"
            "  sheets:               {} of the meshes, doubled for their backs\n",
            stats.mSkippedUnknown, stats.mUnskinned, stats.mSkippedEmpty, stats.mUndescribedMaterials,
            stats.mWornOtherwise, sheets);

        if (mRequest.mStops[mAt].mActions.mWalkTwice)
        {
            const Rtx::ExtractionStats& again = owner.getSecondWalkStats();
            mReport += std::format(
                "\nsecond pass over the same graph\n"
                "  new meshes:           {} (should be 0)\n"
                "  new materials:        {} (should be 0)\n"
                "  drawables resolved:   {} to a known mesh\n",
                again.mMeshesAdded, again.mMaterialsAdded, again.mMeshesReused);
        }
    }

    void Session::writeSheet(RtxRenderer& owner, const std::filesystem::path& sheet)
    {
        Resource::ResourceSystem* resources = owner.getResources();
        if (resources == nullptr)
            return;

        const Rtx::SceneDesc& scene = owner.getMirror().getScene();

        Rtx::SceneTextures described;
        described.describeAll(scene, *resources->getImageManager());

        const Rtx::ContactSheet drawn
            = Rtx::writeContactSheet(described.getDescriptions(), sheet, Settings::rtx().mDelight);
        if (drawn.mCount == 0)
        {
            mReport += "the world uses no textures\n";
            mExitStatus = 1;
            return;
        }

        // The sheet carries no lettering, so the order is printed instead: left to right, top to
        // bottom, the way it was drawn.
        const std::span<const VFS::Path::Normalized> paths = scene.getTextures();
        for (std::size_t at = 0; at < paths.size(); ++at)
            mReport += std::format("  {}  {}\n", at, paths[at].value());

        mReport += std::format("wrote {}, {} textures at delight {}\n", Files::pathToUnicodeString(sheet), drawn.mCount,
            static_cast<float>(Settings::rtx().mDelight));
    }

    bool Session::writeView(OffscreenView& view, const int width, const int height, const std::filesystem::path& file)
    {
        view.keepCopy();
        view.redraw();

        const osg::Image* drawn = view.getCopy();
        if (drawn == nullptr)
        {
            mReport += "the picture was not drawn\n";
            mExitStatus = 1;
            return false;
        }

        const auto stride = static_cast<std::size_t>(width) * 4;
        std::vector<std::uint8_t>& pixels = mHeld->mPixels;
        pixels.clear();
        pixels.resize(stride * static_cast<std::size_t>(height));

        for (int row = 0; row < height; ++row)
            std::memcpy(
                pixels.data() + stride * static_cast<std::size_t>(row), drawn->data(0, height - 1 - row), stride);

        Rtx::writePng(file, static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), pixels);
        mReport += std::format("wrote {} {}x{}\n", Files::pathToUnicodeString(file), width, height);

        return true;
    }

    void Session::writeMapTile(RtxRenderer& owner, const std::filesystem::path& file)
    {
        osg::Group* root = owner.getSceneRoot();
        if (root == nullptr)
            return;

        const MWWorld::Ptr player = MWBase::Environment::get().getWorld()->getPlayerPtr();
        const osg::Vec3f stood = player.getRefData().getPosition().asVec3();

        // **The framing `MWRender::LocalMap` uses, because this is the same picture.** One cell
        // across, straight down, under a flat light that makes no shadows: a chart is read for what
        // is where, and the game's compass draws exactly this every frame a player walks.
        OffscreenViewSpec spec{ *root };
        spec.mWidth = sMapTileSide;
        spec.mHeight = sMapTileSide;
        spec.mProjection = OffscreenViewSpec::Orthographic{ .mWidth = static_cast<float>(Constants::CellSizeInUnits),
            .mHeight = static_cast<float>(Constants::CellSizeInUnits) };
        spec.mNear = SceneUtil::sMapNear;
        spec.mFar = sMapFar;
        spec.mClearColour = osg::Vec4f(0.0f, 0.0f, 0.0f, 1.0f);
        spec.mSun = SceneUtil::mapLight();
        spec.mFromWorld = true;

        const std::unique_ptr<OffscreenView> view = owner.createOffscreenView(spec);
        view->setView(osg::Matrixf::lookAt(osg::Vec3f(stood.x(), stood.y(), sMapEyeHeight),
            osg::Vec3f(stood.x(), stood.y(), sMapEyeHeight - 1.0f), osg::Vec3f(0.0f, 1.0f, 0.0f)));

        writeView(*view, spec.mWidth, spec.mHeight, file);
    }

    void Session::writeDoll(RtxRenderer& owner, const std::string& who, const std::filesystem::path& file)
    {
        MWBase::World& world = *MWBase::Environment::get().getWorld();
        const ESM::RefId id = ESM::RefId::stringRefId(who);

        // **Stood in the world and not assembled beside it.** `MWRender::NpcAnimation` is what
        // dresses a body out of the parts a race calls for, equips what the record carries and
        // finds the bone a weapon hangs on — and it needs a live reference to do any of it.
        const MWWorld::Ptr player = world.getPlayerPtr();
        MWWorld::ManualRef ref(*MWBase::Environment::get().getESMStore(), id, 1);
        const MWWorld::Ptr subject
            = world.placeObject(ref.getPtr(), player.getCell(), player.getRefData().getPosition());

        if (subject.isEmpty())
        {
            mReport += std::format("no NPC record is called \"{}\"\n", who);
            mExitStatus = 1;
            return;
        }

        InventoryPreview preview(owner, owner.getResources(), subject);
        preview.rebuild();
        preview.redraw();

        // **Through the texture the GUI already draws from**, which is the slot the trace wrote
        // into. `MyGUIRtx::Texture` is what this renderer's MyGUI backend hands out, and its slot
        // is the one thing about it a file needs.
        auto& texture = static_cast<MyGUIRtx::Texture&>(preview.getTexture());
        owner.getBackend().readGuiTexture(texture.getSlot(), mHeld->mPixels);

        const auto width = static_cast<std::uint32_t>(preview.getTextureWidth());
        const auto height = static_cast<std::uint32_t>(preview.getTextureHeight());
        Rtx::writePng(file, width, height, mHeld->mPixels);
        mReport += std::format("wrote {} {}x{}\n", Files::pathToUnicodeString(file), width, height);
    }

    void Session::reportFound(RtxRenderer& owner, const std::string& needle)
    {
        const Rtx::SceneDesc& scene = owner.getMirror().getScene();
        const std::span<const VFS::Path::Normalized> paths = scene.getTextures();

        // **Found by texture and reported by placement**, because a mesh carries no name of its own
        // once it is a run of triangles: what a walk keeps is the material it arrived wearing, and a
        // material names the file it samples.
        std::uint32_t met = 0;
        for (const Rtx::MeshInstance& instance : scene.getInstances())
        {
            if (!instance.isPlaced())
                continue;

            if (instance.mMaterial == Rtx::sNoIndex)
                continue;

            const Rtx::Material& material = scene.getMaterials()[instance.mMaterial];
            if (material.mDiffuse == Rtx::sNoIndex)
                continue;

            const std::string_view path = paths[material.mDiffuse].value();
            if (path.find(needle) == std::string_view::npos)
                continue;

            const osg::Vec3f at = instance.mTransform.getTrans();
            mReport += std::format("  {:.0f}, {:.0f}, {:.0f}   {}\n", at.x(), at.y(), at.z(), path);
            ++met;
        }

        mReport += std::format("{} placements wear a texture matching \"{}\"\n", met, needle);
    }

    void Session::runChecks(RtxRenderer& owner)
    {
        const Stop& stop = mRequest.mStops[mAt];

        for (const Check check : stop.mActions.mChecks)
        {
            std::string found;
            const bool held = checkHolds(owner, check, mHeld->mCrossings, mHeld->mSettled, found);

            ++mChecked;
            if (!held)
            {
                ++mFailed;
                mExitStatus = 1;
            }

            mReport += std::format("  {:<20} {:<4} {}\n", checkName(check), held ? "ok" : "FAIL", found);
        }
    }

    void Session::finish()
    {
        mDone = true;

        mReport += Rtx::describeTotal(mPlaces, false);

        if (!mRequest.mHashes.empty())
        {
            mHeld->mHashes.write(mRequest.mHashes);
            mReport += std::format("\nwrote {} frame hashes to {}\n", mHeld->mHashes.frameCount(),
                Files::pathToUnicodeString(mRequest.mHashes));
        }

        if (!mRequest.mAgainst.empty())
        {
            mReport += std::format("\nagainst {}\n", Files::pathToUnicodeString(mRequest.mAgainst));
            for (const Rtx::FrameHashes::ViewDifference& difference : mHeld->mHashes.against(mHeld->mReference))
            {
                mReport += std::format("  {:<28} {}\n", difference.mView, Rtx::describeDifference(difference));
                if (!difference.same())
                    mExitStatus = 1;
            }
        }

        if (!mRequest.mJson.empty())
        {
            mHeader.mSuite = mRequest.mSuite;
            Rtx::writeJson(mRequest.mJson, mHeader, mPlaces);
            mReport += "wrote " + Files::pathToUnicodeString(mRequest.mJson) + '\n';
        }

        // **The way the quit key ends a session, and not `exit`.** A run that tore the process down
        // where it stood would leave the save, the log and the device wherever they happened to be,
        // and the next thing anyone would debug is the session.
        if (mRequest.mQuitAtEnd)
            MWBase::Environment::get().getStateManager()->requestQuit();
    }
}
