#include "standingnote.hpp"

#include <cstdint>
#include <fstream>
#include <ostream>

#include <SDL_keyboard.h>
#include <SDL_scancode.h>

#include <osg/Vec3d>
#include <osg/Vec3f>

#include <apps/openmw/mwbase/environment.hpp>
#include <apps/openmw/mwbase/world.hpp>
#include <apps/openmw/mwrender/camera.hpp>
#include <apps/openmw/mwrender/renderingmanager.hpp>
#include <apps/openmw/mwworld/cell.hpp>
#include <apps/openmw/mwworld/cellstore.hpp>
#include <apps/openmw/mwworld/ptr.hpp>
#include <apps/openmw/mwworld/timestamp.hpp>
#include <components/debug/debugging.hpp>
#include <components/debug/debuglog.hpp>
#include <components/files/conversion.hpp>
#include <components/rtx/skylight.hpp>

#include "run.hpp"

namespace RtxTool
{
    namespace
    {
        /// How far ahead the `look` a run reports points.
        ///
        /// **A landmark's distance rather than a nose's.** The renderer wants a direction; a person
        /// reading `pos` and `look` in `views.cfg` wants to be able to tell where they point, and a
        /// cell is eight thousand units across.
        constexpr double sLookAhead = 1000.0;
    }

    void StandingNote::begin(const Stop& stop)
    {
        Stop& stood = mStood.emplace();
        stood.mName = stop.mName;
        stood.mNote = stop.mNote;
        stood.mStand.mCell = stop.mStand.mCell;

        // Where the player stands is noted with the eye, every frame: `take` says why.
        mNotedCell = ESM::RefId();
    }

    void StandingNote::take()
    {
        MWBase::World& world = *MWBase::Environment::get().getWorld();
        const MWRender::Camera& camera = *world.getRenderingManager()->getCamera();
        const MWWorld::TimeStamp now = world.getTimeStamp();

        const osg::Vec3d at = camera.getPosition();

        // Assigned field by field into the note it already holds, so the weather's string keeps
        // its room from one frame to the next. `begin` made the note.
        Stop& stood = *mStood;
        stood.mStand.mEye = osg::Vec3f(at);

        // **The cell the player stands in now, and not the one the stop began in**: a window walked
        // through a door stands in the interior, whose coordinates are its own, and a note naming
        // the exterior it left puts them somewhere else. Spelt again only where the cell changed,
        // so a frame that stays in one builds no string.
        const MWWorld::CellStore* const standing = world.getPlayerPtr().getCell();
        if (standing != nullptr && standing->getCell()->getId() != mNotedCell)
        {
            const MWWorld::Cell& cell = *standing->getCell();
            stood.mStand.mCell = cellArgument(cell.isExterior(), cell.getGridX(), cell.getGridY(), cell.getNameId());
            mNotedCell = cell.getId();
        }

        // A point far along the direction and not one a unit ahead: a float ulp where Morrowind's
        // cells are is a hundredth of a unit, so two points a unit apart name a direction a fifth
        // of a degree out. A view file holds a `look`, and a landmark's distance is what makes one
        // readable.
        stood.mStand.mLook = osg::Vec3f(at + camera.getOrient() * osg::Vec3d(0.0, sLookAhead, 0.0));
        stood.mSky.mHour = now.getHour();
        stood.mSky.mDay = now.getDay();
        if (!stood.mSky.mWeather.has_value())
            stood.mSky.mWeather.emplace();
        *stood.mSky.mWeather = Rtx::weatherName(static_cast<std::uint32_t>(world.getCurrentWeatherScriptId()));

        // The factor the weather system counts down from one, so what is noted counts up.
        const int arriving = world.getNextWeatherScriptId();
        mArriving = arriving < 0 ? std::string_view() : Rtx::weatherName(static_cast<std::uint32_t>(arriving));
        mCrossed = 1.0f - world.getWeatherTransition();
    }

    void StandingNote::printIfAsked(const std::filesystem::path& keys)
    {
        // **SDL's own key state, and not a script.** The other keys a window answers are named in
        // `keys.lua`, because what they do is turn the world, which only a script may; what this
        // one does is print the session's own note of the frame, which no script can reach. The
        // state array is the engine's, pumped once a frame on this thread.
        const bool down = SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_HOME] != 0;
        if (down && !mPrintKeyHeld)
        {
            Debug::getRawStdout() << describeStanding(*mStood) << std::flush;

            if (!keys.empty())
            {
                std::ofstream(keys, std::ios::app) << '\n' << describeKey(*mStood);
                Log(Debug::Info) << "Ray tracing session: a film's key appended to "
                                 << Files::pathToUnicodeString(keys);
            }
        }
        mPrintKeyHeld = down;
    }

    std::string_view StandingNote::describeTitle()
    {
        // Nothing before a stop's first note: `begin` makes the note empty and `take` fills it on the
        // frame after, so the title of the frame a stop begins on carries nothing rather than a
        // place the run has not stood in yet. The two halves are one note, so the hour standing is
        // the weather standing.
        if (!mStood.has_value() || !mStood->mSky.mHour.has_value())
            return {};

        return writeSkyNote(mTitleNote,
            SkyNote{ .mWeather = mStood->mSky.mWeather.value(),
                .mArriving = mArriving,
                .mCrossed = mCrossed,
                .mHour = *mStood->mSky.mHour });
    }
}
