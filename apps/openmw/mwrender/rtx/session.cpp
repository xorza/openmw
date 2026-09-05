#include "session.hpp"

#include <algorithm>
#include <cmath>
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
#include <components/rtx/frametimes.hpp>
#include <components/rtx/lightbuilder.hpp>
#include <components/rtx/png.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtxbench/framehashes.hpp>
#include <components/rtxbench/gpuclock.hpp>
#include <components/rtxbench/perfcontrol.hpp>

#include "../../mwbase/environment.hpp"
#include "../../mwbase/statemanager.hpp"
#include "../../mwbase/world.hpp"
#include "../../mwworld/cell.hpp"
#include "../../mwworld/cellstore.hpp"
#include "../../mwworld/datetimemanager.hpp"
#include "../../mwworld/globals.hpp"
#include "../../mwworld/ptr.hpp"
#include "../../mwworld/refdata.hpp"

#include "../camera.hpp"
#include "../renderingmanager.hpp"

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
    }

    void installSession(SessionRequest request)
    {
        sInstalled = std::move(request);
    }

    std::optional<SessionRequest> takeInstalledSession()
    {
        std::optional<SessionRequest> taken;
        taken.swap(sInstalled);
        return taken;
    }

    void publishSessionResult(SessionResult result)
    {
        sResult = std::move(result);
    }

    SessionResult takeSessionResult()
    {
        SessionResult taken;
        taken.mPlaces.swap(sResult.mPlaces);
        taken.mReport.swap(sResult.mReport);
        taken.mExitStatus = std::exchange(sResult.mExitStatus, 0);
        return taken;
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

    Session::~Session() = default;

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

        // **Through the global the console writes and not through the clock's own setter**, which
        // is `MWWorld::World`'s alone. `set gamehour to` is the same call, so the hour a stop
        // stands at is the hour a player could have typed.
        if (stop.mSky.mHour.has_value())
            world.setGlobalFloat(MWWorld::Globals::sGameHour, *stop.mSky.mHour);

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
        if (stop.mSky.mHour.has_value() || stop.mSky.mWeather.has_value())
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

        if (stop.mStand.mEye.has_value())
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
    }

    void Session::frame(Rtx::Renderer& renderer, const Rtx::FrameResult& result, const double frameMs,
        const double walkMs, const double placeMs, const bool rebuilt)
    {
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

        if (stop.mActions.mHash)
        {
            renderer.readPixels(mHeld->mPixels);
            mHeld->mHashes.add(stop.mName, drawn, mHeld->mPixels);
        }

        if (drawn < measured)
            return;

        endStop(renderer);
    }

    void Session::endStop(Rtx::Renderer& renderer)
    {
        const Stop& stop = mRequest.mStops[mAt];

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

        SessionResult result;
        result.mExitStatus = mExitStatus;
        result.mPlaces = mPlaces;
        result.mReport = mReport;
        publishSessionResult(std::move(result));

        // **The way the quit key ends a session, and not `exit`.** A run that tore the process down
        // where it stood would leave the save, the log and the device wherever they happened to be,
        // and the next thing anyone would debug is the session.
        if (mRequest.mQuitAtEnd)
            MWBase::Environment::get().getStateManager()->requestQuit();
    }
}
