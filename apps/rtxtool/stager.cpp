#include "stager.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <osg/Vec3f>

#include <apps/openmw/mwbase/environment.hpp>
#include <apps/openmw/mwbase/inputmanager.hpp>
#include <apps/openmw/mwbase/windowmanager.hpp>
#include <apps/openmw/mwbase/world.hpp>
#include <apps/openmw/mwmechanics/creaturestats.hpp>
#include <apps/openmw/mwmechanics/npcstats.hpp>
#include <apps/openmw/mwmechanics/stat.hpp>
#include <apps/openmw/mwrender/renderingmanager.hpp>
#include <apps/openmw/mwworld/cell.hpp>
#include <apps/openmw/mwworld/cellstore.hpp>
#include <apps/openmw/mwworld/class.hpp>
#include <apps/openmw/mwworld/containerstore.hpp>
#include <apps/openmw/mwworld/datetimemanager.hpp>
#include <apps/openmw/mwworld/esmstore.hpp>
#include <apps/openmw/mwworld/globals.hpp>
#include <apps/openmw/mwworld/ptr.hpp>
#include <components/debug/debuglog.hpp>
#include <components/detournavigator/navigator.hpp>
#include <components/detournavigator/waitconditiontype.hpp>
#include <components/esm/attr.hpp>
#include <components/esm/position.hpp>
#include <components/esm/refid.hpp>
#include <components/esm3/loadregn.hpp>
#include <components/esm3/loadskil.hpp>
#include <components/misc/rng.hpp>
#include <components/rtx/skylight.hpp>

#include "model/benchrun.hpp"

namespace RtxTool
{
    namespace
    {
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

    std::optional<std::string> Stager::stage(const Stop& stop, const SessionRequest& request) const
    {
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
                return "no cell is called \"" + stop.mStand.mCell + '"';

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

        // **Seeded again here, where the stop's frames begin**: `SessionRequest::mRandomSeed`
        // says why the seed the engine started with is not enough. Both generators, because the
        // world keeps one of its own beside the process's — `AiWander`, `Combat`,
        // `CharacterController` and `WeatherManager` roll on `World::getPrng` — and a stop that
        // seeded only the process's would stand its actors and strike its lightning wherever every
        // stop before it left that stream.
        Misc::Rng::init(request.mRandomSeed);
        world.getPrng().seed(request.mRandomSeed);

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

        MWBase::Environment::get().getWindowManager()->setHudVisibility(request.mHud);

        // The same switch the camera script reads (`Player.CONTROL_SWITCH.VanityMode`), so a
        // content script may still turn it either way.
        MWBase::Environment::get().getInputManager()->toggleControlSwitch("vanitymode", request.mVanity);

        if (stop.mSchedule.mFreeCamera)
        {
            // **The walls come off, because a view file names where a camera stands.** Half of them
            // are inside a rock or over the sea, and a body dropped there either falls or cannot be
            // put there at all. Before `CameraDriver::begin` reads where the player stands, because
            // turning collision on calls `World::adjustPosition`, which drops the player onto the
            // ground.
            turnCollisionOff(world);
            boostPlayer();
        }
        else if (stop.mStand.mEye.has_value() && stop.mSchedule.mTrack.has_value())
        {
            // **A take's body goes where its camera flies, through whatever is in the way**, so the
            // cells stream in around the camera and not around a body stopped by a hill it flew
            // over. **The game's clock stops**, because the track states the hour on every frame:
            // at the game's time scale of thirty, a clock left running adds half a second of world
            // time to every frame of sixty.
            turnCollisionOff(world);
            world.getTimeManager()->setGameTimeScale(0.0f);
        }

        // **The navmesh whole before the first frame.** Its tiles are built on a thread of their
        // own, and an actor told to go somewhere paths over the tiles there are when it asks: a
        // tile landing a frame earlier or later is a different path, and a different place on
        // every frame after. Waited for here and not every frame, because a route's cells bring
        // tiles with them and a frame that waited for those would be measuring the navmesh.
        if (DetourNavigator::Navigator* navigator = world.getNavigator())
            navigator->wait(DetourNavigator::WaitConditionType::allJobsDone, nullptr);

        // **A stop is a discontinuity, and only a worldspace change says so on its own.** A
        // teleport from Balmora to Vivec stays in one worldspace, so nothing tells the renderer its
        // history describes somewhere else — and the exposure adapts toward its measurement over
        // seconds rather than taking it, so a room drawn after a noon exterior opens at the
        // exterior's brightness. The warm-up absorbs the frame it costs.
        forgetHistory();

        return std::nullopt;
    }

    void Stager::forgetHistory()
    {
        MWBase::Environment::get().getWorld()->getRenderingManager()->notifyWorldSpaceChanged();
    }

    void Stager::setWeather(MWBase::World& world, const std::string_view name)
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

    void Stager::boostPlayer()
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

    void Stager::turnCollisionOff(MWBase::World& world)
    {
        // **Toggled until it is off, because the call reports rather than sets.** `tcl` is the same
        // call, and a session that had already used it would otherwise turn collision back on.
        if (world.toggleCollisionMode())
            world.toggleCollisionMode();
    }
}
