#include "session.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <osg/Vec3d>
#include <osg/Vec3f>

#include <apps/openmw/mwbase/environment.hpp>
#include <apps/openmw/mwbase/inputmanager.hpp>
#include <apps/openmw/mwbase/statemanager.hpp>
#include <apps/openmw/mwbase/windowmanager.hpp>
#include <apps/openmw/mwbase/world.hpp>
#include <apps/openmw/mwmechanics/creaturestats.hpp>
#include <apps/openmw/mwmechanics/npcstats.hpp>
#include <apps/openmw/mwmechanics/stat.hpp>
#include <apps/openmw/mwrender/camera.hpp>
#include <apps/openmw/mwrender/renderingmanager.hpp>
#include <apps/openmw/mwrender/rtx/rtxrenderer.hpp>
#include <apps/openmw/mwworld/cell.hpp>
#include <apps/openmw/mwworld/cellstore.hpp>
#include <apps/openmw/mwworld/class.hpp>
#include <apps/openmw/mwworld/containerstore.hpp>
#include <apps/openmw/mwworld/datetimemanager.hpp>
#include <apps/openmw/mwworld/esmstore.hpp>
#include <apps/openmw/mwworld/globals.hpp>
#include <apps/openmw/mwworld/ptr.hpp>
#include <apps/openmw/mwworld/timestamp.hpp>
#include <components/debug/debuglog.hpp>
#include <components/esm/attr.hpp>
#include <components/esm/position.hpp>
#include <components/esm/refid.hpp>
#include <components/esm3/loadregn.hpp>
#include <components/esm3/loadskil.hpp>
#include <components/rtx/framespend.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtx/skylight.hpp>
#include <components/rtxbench/benchrecord.hpp>
#include <components/rtxbench/benchspec.hpp>
#include <components/rtxbench/framehashes.hpp>
#include <components/rtxbench/frametimes.hpp>
#include <components/rtxbench/gpuclock.hpp>

namespace RtxTool
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

        /// What every attribute and skill of a flown body is set to. Past the hundred the game
        /// levels toward, so nothing a stat gates is out of reach.
        constexpr float sBoostedStat = 255.0f;

        /// Speed alone, which is the one attribute a session feels. `Npc::getWalkSpeed` reads it
        /// as `fMinWalkSpeed` plus a hundredth of the walk range per point, which under vanilla's
        /// settings is a hundred units a second plus one per point: a walk of 2100, and a run of
        /// 4.3 times that with Athletics at the figure above.
        constexpr float sBoostedSpeed = 2000.0f;

        constexpr int sBoostedLevel = 255;
        constexpr int sBoostedGold = 10'000'000;
    }

    Session::Session(Rtx::SessionRequest request)
        : mRequest(std::move(request))
        , mProfiling(mRequest.mPerfControl)
    {
        if (!mRequest.mAgainst.empty())
            mRecord.readReference(mRequest.mAgainst);

        std::uint32_t longest = 0;
        for (const Rtx::Stop& stop : mRequest.mStops)
            longest = std::max(longest, stop.mSchedule.mSpec.getMeasured());

        // Reserved once at the longest stop's length, so no measured frame grows a vector — a
        // benchmark that stops to reallocate is measuring its own allocator.
        mProgress.mSamples.reserve(longest);

        mRecord.reserve(mRequest.mStops.size());

        if (mRequest.mStops.empty())
            mDone = true;
    }

    Rtx::SessionResult Session::describe() const
    {
        return mRecord.describe(mStood.has_value() ? &*mStood : nullptr);
    }

    void Session::noteStanding()
    {
        MWBase::World& world = *MWBase::Environment::get().getWorld();
        const MWRender::Camera& camera = *world.getRenderingManager()->getCamera();
        const MWWorld::TimeStamp now = world.getTimeStamp();

        const osg::Vec3d at = camera.getPosition();

        // Assigned field by field into the note it already holds, so the weather's string keeps
        // its room from one frame to the next. `beginStop` made the note.
        Rtx::Stop& stood = *mStood;
        stood.mStand.mEye = osg::Vec3f(at);

        // The direction and not a point on it, for the reason `Rtx::makeCamera` gives — but a view
        // file holds a `look`, and a landmark's distance is what makes one readable.
        stood.mStand.mLook = osg::Vec3f(at + camera.getOrient() * osg::Vec3d(0.0, sLookAhead, 0.0));
        stood.mSky.mHour = now.getHour();
        stood.mSky.mDay = now.getDay();
        if (!stood.mSky.mWeather.has_value())
            stood.mSky.mWeather.emplace();
        *stood.mSky.mWeather = Rtx::weatherName(static_cast<std::uint32_t>(world.getCurrentWeatherScriptId()));
    }

    void Session::abandon(const std::string_view why)
    {
        Log(Debug::Error) << "Ray tracing session: " << why;
        mRecord.fail();
        mDone = true;
        MWBase::Environment::get().getStateManager()->requestQuit();
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
        MWRender::Camera* camera = MWBase::Environment::get().getWorld()->getRenderingManager()->getCamera();

        // **A static camera and not the player's own.** Nothing tracks the body, nothing rotates
        // to its facing and nothing casts a ray to keep the eye out of a wall, which is what a view
        // file's coordinates mean. It does not hold on its own, for the reason `Session::aim` gives.
        camera->setMode(MWRender::Camera::Mode::Static);
        camera->setStaticPosition(osg::Vec3d(eye));

        // The body's rotation, negated into the camera's own angles the way
        // `Camera::rotateCameraToTrackingPtr` negates a tracked body's.
        const osg::Vec3f rotation = Rtx::Stand{ .mCell = {}, .mEye = eye, .mLook = look }.getRotation();
        camera->setPitch(-rotation.x(), true);
        camera->setYaw(-rotation.z(), true);
    }

    void Session::setWeather(MWBase::World& world, const std::string_view name)
    {
        const std::optional<std::uint32_t> named = Rtx::weatherIndex(name);
        if (!named.has_value())
        {
            Log(Debug::Warning) << "Ray tracing session: no weather is called \"" << name << '"';
            return;
        }

        world.changeWeather(world.getPlayerPtr().getCell()->getCell()->getRegion(),
            ESM::Weather::indexToRefId(static_cast<int>(*named)));
    }

    void Session::boostPlayer()
    {
        const MWWorld::Ptr player = MWBase::Environment::get().getWorld()->getPlayerPtr();
        const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
        MWMechanics::CreatureStats& creature = player.getClass().getCreatureStats(player);
        MWMechanics::NpcStats& npc = player.getClass().getNpcStats(player);

        // **The base, with the modifier and the damage cleared**, which is what `setattribute` does
        // and what leaves a fortify or a drain from the start of the game out of the figure.
        for (const ESM::Attribute& attribute : store.get<ESM::Attribute>())
        {
            MWMechanics::AttributeValue value = creature.getAttribute(attribute.mId);
            value.setBase(attribute.mId == ESM::Attribute::Speed ? sBoostedSpeed : sBoostedStat, true);
            creature.setAttribute(attribute.mId, value);
        }

        for (const ESM::Skill& skill : store.get<ESM::Skill>())
            npc.getSkill(skill.mId).setBase(sBoostedStat, true);

        creature.setLevel(sBoostedLevel);

        // Weightless, so no amount of it encumbers the body it is given to.
        player.getClass().getContainerStore(player).add(MWWorld::ContainerStore::sGoldId, sBoostedGold);
    }

    void Session::standWhereThePlayerIs()
    {
        // The reference lives in the cell store rather than in the `Ptr`, which is what the named
        // player says: the position outlives the handle it was reached through.
        const MWWorld::Ptr player = MWBase::Environment::get().getWorld()->getPlayerPtr();
        const ESM::Position& stood = player.getRefData().getPosition();

        const osg::Vec3f eye(stood.pos[0], stood.pos[1], stood.pos[2]);
        mProgress.standAt(eye, eye + Rtx::Stand::forwardOf(osg::Vec3f(stood.rot[0], stood.rot[1], stood.rot[2])));
    }

    void Session::forgetHistory()
    {
        MWBase::Environment::get().getWorld()->getRenderingManager()->notifyWorldSpaceChanged();
    }

    void Session::beginStop()
    {
        const Rtx::Stop& stop = mRequest.mStops[mAt];
        MWBase::World& world = *MWBase::Environment::get().getWorld();

        // **First, because everything below writes into it.** A stop's progress is one object so
        // that a field added to it is reset here whether or not its author remembered to.
        mProgress.restart();

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
                abandon("no cell is called \"" + stop.mStand.mCell + '"');
                return;
            }

            // **Where the eye goes and not where the cell centres, where the stop says.** The
            // position the world found is what stands a player in the cell; a view names the spot
            // its picture is of, and the ring is the same ring either way. The body faces the look
            // as well, because a window is the player's own camera and that camera faces what the
            // body does.
            if (stop.mStand.mEye.has_value())
            {
                where.pos[0] = stop.mStand.mEye->x();
                where.pos[1] = stop.mStand.mEye->y();
                where.pos[2] = stop.mStand.mEye->z();

                const osg::Vec3f rotation = stop.mStand.getRotation();
                where.rot[0] = rotation.x();
                where.rot[1] = rotation.y();
                where.rot[2] = rotation.z();
            }

            world.changeToCell(found, where, true);
        }
        else if (stop.mStand.mEye.has_value())
        {
            world.moveObject(world.getPlayerPtr(), *stop.mStand.mEye, true, true);
            world.rotateObject(world.getPlayerPtr(), stop.mStand.getRotation());
        }

        // **Through the globals the console writes and not through the clock's own setters**, which
        // are `MWWorld::World`'s alone. `set gamehour to` and `set day to` are the same two calls,
        // so a stop stands at an hour and a date a player could have typed.
        if (stop.mSky.mHour.has_value())
            world.setGlobalFloat(MWWorld::Globals::sGameHour, *stop.mSky.mHour);

        if (stop.mSky.mDay.has_value())
            world.setGlobalInt(MWWorld::Globals::sDay, *stop.mSky.mDay);

        if (stop.mSky.mWeather.has_value())
            setWeather(world, *stop.mSky.mWeather);

        // **Settled rather than crossed into**, which is what the game does when a player sleeps:
        // a stop asked to stand under a sky stands under it from its first frame rather than four
        // seconds later. A run that turns its sky asks for the transition instead.
        if (stop.mSky.mHour.has_value() || stop.mSky.mDay.has_value() || stop.mSky.mWeather.has_value())
            world.advanceTime(0.0, false);

        if (!stop.mSky.mTurnThrough.empty())
            setWeather(world, stop.mSky.mTurnThrough.front());

        // **The clock stops after the world has been moved and not before.** A frozen stop is a
        // reference: nothing animates, so a frame traced many times is the same frame and an
        // accumulated picture converges on the integral rather than on the animation.
        world.getTimeManager()->setSimulationTimeScale(stop.mSchedule.mFrozen ? 0.0f : 1.0f);

        // **Nothing a session does to the player may kill them.** A route flies the body across
        // the world at whatever speed its view names and leaves it wherever the line ends, and the
        // player dies of that: measured on `island-crossing`, the game reached `State_Ended` on the
        // frame the route arrived. A dead player ends the game, which strands every stop after this
        // one. `tgm` is the same call, so a run stands where a player who typed it would.
        if (!world.getGodModeState())
            world.toggleGodMode();

        MWBase::Environment::get().getWindowManager()->setHudVisibility(mRequest.mHud);

        // The same switch the camera script reads (`Player.CONTROL_SWITCH.VanityMode`), so a
        // content script may still turn it either way.
        MWBase::Environment::get().getInputManager()->toggleControlSwitch("vanitymode", mRequest.mVanity);

        const MWWorld::Ptr player = world.getPlayerPtr();
        mProgress.mCell = player.getCell();

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

            boostPlayer();
        }
        else if (stop.mStand.mEye.has_value())
        {
            mProgress.standAt(*stop.mStand.mEye, stop.mStand.getLook());

            // **Here as well as every frame**, because a frame drawn between this and the first
            // `aim` would be drawn from wherever the last stop left the camera.
            aim();
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

        // What a launcher writes the place down under; where the eye is goes in every frame.
        Rtx::Stop& stood = mStood.emplace();
        stood.mName = stop.mName;
        stood.mNote = stop.mNote;
        stood.mStand.mCell = stop.mStand.mCell;

        // A stop from a save names no cell, so the one the player stands in is written down
        // instead, spelt as `--cell` takes it: a grid pair outdoors and the name indoors.
        if (stood.mStand.mCell.empty())
        {
            const MWWorld::Cell& cell = *world.getPlayerPtr().getCell()->getCell();
            stood.mStand.mCell = cell.isExterior()
                ? std::to_string(cell.getGridX()) + "," + std::to_string(cell.getGridY())
                : std::string(cell.getNameId());
        }

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
        // **Measured from `mProgress.mFlown` and never from the player**, which says why.
        osg::Vec3f along = route.mTo.has_value() ? *route.mTo - mProgress.mFlown
                                                 : Rtx::Stand::forwardOf(osg::Vec3f(0.0f, 0.0f, stood.rot[2]));

        const float left = along.length();
        if (route.mTo.has_value() && left <= 0.0f)
        {
            mProgress.mArrived = true;
            return;
        }

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
        mProgress.mFlown += along * step;

        // **`moveObjectBy` and not `moveObject`, because the player is an actor.** The actor's
        // position lives in the physics world as well, and a move that writes only the world's
        // copy is written back over it on the next step.
        world.moveObjectBy(player, mProgress.mFlown - standing, true);
    }

    void Session::aim()
    {
        const Rtx::Stop& stop = mRequest.mStops[mAt];
        if (stop.mSchedule.mFreeCamera || !stop.mStand.mEye.has_value())
            return;

        const std::optional<Rtx::Route>& route = stop.mSchedule.mRoute;
        const osg::Vec3f look = route.has_value() && route->mLookTo.has_value()
            ? *route->mLookTo
            : mProgress.mFlown + (mProgress.mFromLook - mProgress.mFrom);

        aimCamera(mProgress.mFlown, look);
    }

    void Session::turnWeather()
    {
        const std::vector<std::string>& through = mRequest.mStops[mAt].mSky.mTurnThrough;
        if (through.size() < 2)
            return;

        mProgress.mTurned += 1.0f / sTurnFrames;
        if (mProgress.mTurned < 1.0f)
            return;

        mProgress.mTurned = 0.0f;
        mProgress.mTurnedTo = (mProgress.mTurnedTo + 1) % through.size();

        setWeather(*MWBase::Environment::get().getWorld(), through[mProgress.mTurnedTo]);
    }

    std::optional<std::uint32_t> Session::getSampleFrame() const
    {
        if (mDone || !mStarted)
            return std::nullopt;

        return mProgress.mSeen;
    }

    std::uint32_t Session::getAccumulated() const
    {
        if (mDone || !mStarted)
            return 0;

        const Rtx::Stop& stop = mRequest.mStops[mAt];
        const std::uint32_t warmup = stop.mSchedule.mSpec.getWarmup();
        if (stop.mSchedule.mAccumulate == 0 || mProgress.mSeen < warmup)
            return 0;

        // **Counted from the first measured frame**, because the warm-up is the world arriving and
        // the card coming off its idle clock. Averaging those in would put a picture of a
        // half-built cell into the reference.
        return mProgress.mSeen - warmup + 1;
    }

    void Session::beforeFrame()
    {
        if (mDone)
            return;

        // **A game that has ended cannot be flown any further, and a session that waited for one
        // would wait for ever.** Every stop after this one is unreachable, so the run says what
        // happened and stops rather than drawing the same frame until somebody kills it. What ends
        // a game here is the player dying, which `beginStop` turns god mode on to prevent — this is
        // for whatever else might.
        if (MWBase::Environment::get().getStateManager()->getState() == MWBase::StateManager::State_Ended)
        {
            abandon(std::format("the game ended during stop {} of {}, so no place after it can be reached", mAt + 1,
                mRequest.mStops.size()));
            return;
        }

        if (!isPlaying())
            return;

        if (!mStarted)
        {
            beginStop();
            return;
        }

        // **The reset stands until a frame has been counted, and the one `beginStop` issued is not
        // enough.** A stop opens with frames nobody counts: the first trace after the teleport has
        // no predecessor to be timed against, so `frame` is never reached for it and `mProgress.mSeen` stays
        // at nought. The exposure adapts on every one of them all the same, and how many there are
        // is a question about how long the world took to load rather than one the schedule answers
        // — so two runs began counting from two exposures and drew the first thirty frames
        // differently, with the same scene behind them. Reissued here, the last reset lands on the
        // frame that becomes the first counted one, whatever went before it.
        //
        // **Both calls, and neither is the other's spare.** `beginStop` resets because a teleport
        // is a discontinuity and the frames it opens with are drawn on a screen. This resets
        // because those frames are not measured, and a measured run may not depend on them.
        if (mProgress.mSeen == 0)
            forgetHistory();

        // **The route runs over the measured frames and not the warm-up.** Warming up is the GPU
        // coming off its idle clock; flying during it would start the measurement partway along
        // and leave the first crossing outside the numbers.
        if (mProgress.mSeen >= mRequest.mStops[mAt].mSchedule.mSpec.getWarmup())
        {
            fly();
            turnWeather();
        }

        // **After the route has stepped and on every frame, warm-up included.** `Session::aim`
        // says why once is not enough; the warm-up frames stand at the route's start, which is
        // where `mFlown` still is.
        aim();

        // **After the schedule has moved, because the note is of the frame about to be drawn.** The
        // route flies the eye and the turn crosses the sky above it, both between this call and the
        // trace — so a note taken before them describes a camera under a sky that no frame ever
        // used. The last one taken is what `RunRecord::describe` publishes.
        noteStanding();
    }

    bool Session::wantsSecondWalk() const
    {
        return !mDone && mStarted && mRequest.mStops[mAt].mActions.mWalkTwice;
    }

    bool Session::wantsFrameCopy() const
    {
        return !mDone && mStarted && mRequest.mStops[mAt].mActions.mHash;
    }

    void Session::frame(const MWRender::FrameContext& context, const MWRender::FrameReport& report)
    {
        Rtx::Renderer& renderer = context.mRenderer.getBackend();
        const double frameMs = report.mFrameMs;

        if (mDone || !mStarted)
            return;

        const Rtx::Stop& stop = mRequest.mStops[mAt];
        const std::uint32_t warmup = stop.mSchedule.mSpec.getWarmup();
        const std::uint32_t measured = stop.mSchedule.mSpec.getMeasured();

        if (mProgress.mSeen == warmup)
        {
            // **Sampled through the measured frames and not at their ends.** Two readings bound
            // nothing: the ends of a place agree to within a couple of per cent while the card
            // moves a fifth of its clock between them, and a leg that lost its clock then reads
            // like a leg that lost its speed. `Rtx::ClockWatch` says what the sampling costs.
            mClockWatch.start();
            mProfiling.enable();
        }

        ++mProgress.mSeen;

        if (mProgress.mSeen <= warmup)
            return;

        mProgress.mSamples.add(frameMs, report.mSpend);
        mProgress.mSamples.addWait(report.mResult->mWaitMs);
        mProgress.mOverlap.add(report.mResult->mInFlight);
        mProgress.mGpu.add(report.mResult->mGpu.spans());
        mProgress.mWallMs += frameMs;

        // **Counted here and not where the route moved**, because a crossing is a dropped frame and
        // this is where what it dropped is known. The move pulls the next ring in and that read
        // lands in the frame after it — which is the frame that arrives here standing in a cell it
        // was not drawn in last time, and the frame that paid for it.
        //
        // **The whole frame goes in as the read**, because the game gives no split: the ring
        // arrives on the loading threads, and what a crossing costs here is the frame that dropped.
        if (const void* cell = MWBase::Environment::get().getWorld()->getPlayerPtr().getCell();
            mProgress.mCell != nullptr && cell != mProgress.mCell)
        {
            mProgress.mCrossings.add(report.mRebuilt, frameMs);
            mProgress.mCell = cell;
        }

        const Rtx::FrameExtents extents = renderer.getExtents();
        const double traced = static_cast<double>(extents.mRenderWidth) * extents.mRenderHeight;
        if (traced > 0.0)
            mProgress.mHitPercent = static_cast<double>(report.mResult->mHits) / traced * 100.0;

        const std::uint32_t drawn = mProgress.mSeen - warmup;

        // The scene now, which is this frame's, and the picture that came back with the report,
        // which is an earlier frame's: the two halves of a row meet through the number the
        // backend gave the frame. A frame the warm-up drew has no row and its picture is dropped.
        if (stop.mActions.mHash)
        {
            Rtx::FrameHashes& hashes = mRecord.getHashes();
            hashes.note(stop.mName, drawn, report.mFrame, mDigester.digest(context.mScene));
            if (!report.mResult->mPixels.empty())
                hashes.picture(report.mResult->mFrame, report.mResult->mPixels);
        }

        if (drawn < measured && !mProgress.mArrived)
            return;

        endStop(context, report);
    }

    void Session::endStop(const MWRender::FrameContext& context, const MWRender::FrameReport& report)
    {
        const Rtx::Stop& stop = mRequest.mStops[mAt];
        Rtx::Renderer& renderer = context.mRenderer.getBackend();

        mProfiling.disable();

        // After the frames and not before them, so the last spawn it costs is outside the run it
        // describes.
        mProgress.mClock = mClockWatch.stop();

        // The last frame's picture is still on the queue; a stop that hashes waits it out here,
        // where a drain is a stop's to pay and never a frame's.
        if (stop.mActions.mHash)
            while (const std::optional<Rtx::FrameResult> finished = renderer.finishFrame())
                if (!finished->mPixels.empty())
                    mRecord.getHashes().picture(finished->mFrame, finished->mPixels);

        const Rtx::FrameExtents extents = renderer.getExtents();

        if (mRecord.empty())
        {
            // **Taken at the first stop, because every stop of a run is traced by one renderer.**
            // What the record's header states is the configuration the whole run stood under. The
            // upscaling is the frame's own answer — the pair the renderer resolved this frame — and
            // not the renderer's mode alone.
            Rtx::BenchHeader& header = mRecord.getHeader();
            header.mExtents = extents;
            header.mUpscaling = report.mReconstruction.mUpscaling;
            header.mValidating = renderer.isValidating();
            header.mMeasured = stop.mSchedule.mSpec.getMeasured();
            header.mWarmup = stop.mSchedule.mSpec.getWarmup();
        }

        mWriter.write(context, report, stop.mActions,
            StopFacts{ .mCrossings = mProgress.mCrossings, .mStand = stop.mStand, .mOverlap = mProgress.mOverlap },
            mRecord);

        Rtx::BenchPlace place;
        place.mView = stop.mName;
        place.mCell = stop.mStand.mCell;
        place.mNote = stop.mNote;
        place.mHour = MWBase::Environment::get().getWorld()->getTimeStamp().getHour();
        place.mWeather = stop.mSky.mWeather.value_or(std::string());
        place.mFrames = mProgress.mSamples.size();
        place.mWallSeconds = mProgress.mWallMs / 1000.0;
        for (std::size_t at = 0; at < Rtx::sTimingCount; ++at)
            place.mRows[at] = Rtx::summarise(mProgress.mSamples.mRows[at]);
        place.mClock = mProgress.mClock;
        place.mHitPercent = mProgress.mHitPercent;
        place.mCrossings = mProgress.mCrossings;
        place.mOverlap = mProgress.mOverlap;

        // How much of the line between the two ends the route flew, where it named both: a run
        // that ended short measured a shorter journey than its name says.
        if (const std::optional<Rtx::Route>& route = stop.mSchedule.mRoute; route.has_value() && route->mTo.has_value())
        {
            const float whole = (*route->mTo - mProgress.mFrom).length();
            if (whole > 0.0f)
                place.mTravelled = std::clamp((mProgress.mFlown - mProgress.mFrom).length() / whole, 0.0f, 1.0f);
        }
        place.mScene = renderer.getSceneStats();
        place.mMemory = renderer.getMemoryReport();

        const std::span<const Rtx::GpuZone> zones = mProgress.mGpu.summariseZones();
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
