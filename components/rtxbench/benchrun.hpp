#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <osg/Vec3f>

#include <components/rtx/renderer.hpp>

#include "benchrecord.hpp"
#include "benchspec.hpp"

namespace Rtx
{
    /// One thing a run asserts about what the renderer was handed or what it drew, of the running
    /// game and never of a staged world, which reads its cells and dresses its people by rules of
    /// its own.
    enum class Check
    {
        /// A second walk over the same graph adds no mesh and no material: the property the
        /// incremental mirror rests on, and the only way to ask it is to ask twice.
        WalkTwice,

        /// Every placement wears a material something described. A placement wearing nothing is a
        /// surface the extractor could not read, which reaches the screen as grey.
        SurfacesDescribed,

        /// A room holds lights to cast. Asked of an interior and answered yes by every exterior,
        /// because a hillside at noon places none; a room that placed none looks dark rather than
        /// faulty.
        LightsPlaced,

        /// A route crossed cell boundaries, and not every crossing had to rebuild. An append and a
        /// rebuild are an order of magnitude apart.
        CrossingsAppend,

        /// An exterior's ground reaches past the square the simulation holds. `Rtx::CellRing`
        /// stands it off the land records, and a world stopping at the active grid looks like a
        /// short view rather than a fault.
        GroundReaches,

        /// Every cell of the reach stands its ground, one placement a cell. A cell the ring did not
        /// stand is a hole, under the player's feet as readily as at the horizon.
        GroundStands,

        /// No two lights stand at the same point. The lamps of unloaded cells are read out of the
        /// content files, and a cell that then loads brings its own copy of each.
        LightsNotDoubled,

        /// Every texture the scene named could be read; an unreadable one is drawn grey.
        TexturesReadable,

        /// The frame was drawn from the camera the stop asked for, because a camera something else
        /// moved gives figures that look reasonable. Answered yes by a stop that named no camera,
        /// by a free-camera stop, and by a routed one.
        CameraStands,

        /// Two frames were in flight at every submit. The ring is sized for two and the game
        /// keeps two, so a submit that found one is a wait somebody put back — the 0.9 ms a frame
        /// the device idled for the whole of this fork's life before `Renderer::collectFrame`.
        /// Asked of a place that stands still, because an arrival drains the ring by design.
        FramesOverlap,
    };

    /// What a check is called on a command line and in a report.
    std::string_view checkName(Check check);

    /// Every check there is, in the order they are run.
    std::span<const Check> everyCheck();

    /// Where a stop stands: a cell, and where the eye is inside it.
    struct Stand
    {
        /// The cell to teleport to, as Morrowind addresses one: a pair of integers is an exterior,
        /// anything else is an interior's name. Empty stays wherever the game already is. The
        /// spelling and not an id, because `MWBase::World::findExteriorPosition` is what turns one
        /// into the other, and a stop then stands where a player typing `coc` would.
        std::string mCell;

        /// Where the eye goes and what it looks at. Both left out leaves the player where the cell
        /// put them and their own camera alone.
        std::optional<osg::Vec3f> mEye;
        std::optional<osg::Vec3f> mLook;

        /// The point the eye faces: `mLook`, or due north where it names nothing or the eye itself,
        /// because a direction of no length aims nothing. One answer, because `RtxTool::Session`
        /// aims at it and `Check::CameraStands` asserts the camera reached it. Only for a stand that
        /// names an eye.
        osg::Vec3f getLook() const;
    };

    /// What the sky does at a stop, asked of the game's own weather system rather than derived.
    /// Named for the stop, because `Sky` is a namespace `Rtx` names.
    struct StopSky
    {
        std::optional<float> mHour;

        /// Which day of Morrowind's calendar, counted from the one a new game begins on. Only the
        /// moons read it: a phase runs on a three-day cycle and no hour can stand for a date.
        std::optional<int> mDay;

        /// A weather as the content files spell it: `Clear`, `Overcast`, `Thunderstorm`. Set
        /// immediately, so a stop stands under it from its first frame.
        std::optional<std::string> mWeather;

        /// Weathers to turn the sky through while the stop runs, in order and round again, as
        /// transitions: what the renderer has to survive is an emitter freed on an ordinary frame.
        /// Asking for it stops the run being a benchmark.
        std::vector<std::string> mTurnThrough;
    };

    /// Where a stop flies to, and how fast. A route is what puts a cell arriving into a
    /// measurement at all.
    struct Route
    {
        /// Where the eye ends and what it looks at there; both left out flies forwards. Empty by
        /// their own initializer, because a designated initializer that skips one reads to GCC as a
        /// short aggregate.
        std::optional<osg::Vec3f> mTo = std::nullopt;
        std::optional<osg::Vec3f> mLookTo = std::nullopt;

        /// World units a second. A Morrowind exterior cell is 8,192 across.
        float mSpeed = 0.0f;
    };

    /// How long a stop runs and what moves while it does.
    struct Schedule
    {
        /// How long it runs and how much of it is thrown away first.
        BenchSpec mSpec;

        std::optional<Route> mRoute;

        /// How many differently-seeded frames to average into one picture, or nought for none. A
        /// converged reference is the only ground truth a sampled renderer has: error falls as the
        /// square root of this, a hundred is a clean picture and a thousand is a reference.
        std::uint32_t mAccumulate = 0;

        /// Whether the world's clock is held still while the stop runs, so a still frame traced
        /// many times is the same frame. `DateTimeManager::setSimulationTimeScale` is where it
        /// lands, so nothing in the world moves.
        bool mFrozen = false;

        /// Whether the player keeps their own camera and collision: a session somebody flies. A
        /// view's coordinates are where a camera stands and not where a body fits, so the walls come
        /// off with it.
        bool mFreeCamera = false;
    };

    /// What a stop writes, and when.
    struct Actions
    {
        /// Where the last measured frame is written as a PNG, or empty for none.
        std::filesystem::path mCapture;

        /// Whether the scene the renderer was handed is reported: what it holds, what it could not
        /// place, and one number for the whole of it.
        bool mDigest = false;

        /// Whether the same graph is walked a second time, so what that added can be asked about.
        /// The largest cost a frame has, so only a stop that asked pays for it.
        bool mWalkTwice = false;

        /// Where every texture the scene holds is written, vanilla beside de-lit, as one sheet.
        std::filesystem::path mSheet;

        /// Where one local-map tile of the place is written, framed the way the game's own compass
        /// frames one.
        std::filesystem::path mMapTile;

        /// Whose inventory doll to write, and where: a picture of a subject and not of the world,
        /// assembled by `MWRender::NpcAnimation` and traced against a scene of its own.
        struct Doll
        {
            std::string mWho;
            std::filesystem::path mFile;
        };
        std::optional<Doll> mDoll;

        /// A word to look for among the textures the scene around this place is wearing — what a
        /// frame would trace, rather than what the content files say stands near.
        std::string mFind;

        /// Whether every measured frame is read back and hashed, which serialises every frame
        /// against the device and so stops the run being a benchmark.
        bool mHash = false;

        /// What this stop asserts. Empty for a stop that only draws.
        std::vector<Check> mChecks;
    };

    /// One place a run visits, and everything that is true of it.
    struct Stop
    {
        /// What the report and the hashes call it. A view id where the run came from a view file.
        std::string mName;

        /// What the report prints beside the name.
        std::string mNote;

        Stand mStand;
        StopSky mSky;
        Schedule mSchedule;
        Actions mActions;
    };

    /// A whole run, as one description, filled by a launcher and read by the renderer.
    struct SessionRequest
    {
        std::vector<Stop> mStops;

        /// Whether the run keeps its window hidden, which saves a present per frame and nothing else.
        bool mHeadless = true;

        /// Whether the game's HUD is drawn over the picture. Off by default: a picture is of the
        /// world, and the bars and the compass are the played game's.
        bool mHud = false;

        /// Whether the run ends the session when its last stop does. False is a window somebody
        /// keeps flying after the schedule has run out.
        bool mQuitAtEnd = true;

        /// Where the run is written as a record, and the hashes it writes and compares. Empty
        /// where none was asked for.
        std::filesystem::path mJson;
        std::filesystem::path mHashes;
        std::filesystem::path mAgainst;

        /// perf's control fifo, or empty where the run is not being profiled.
        std::filesystem::path mPerfControl;

        /// Which suite the stops came from, for the record's own header.
        std::string mSuite;

        /// Whether each hand-over waits for the distant ground it queued, or nothing to let the
        /// frame clock decide. Settled is what makes two processes draw one picture; a run timing
        /// the streaming path says no (`Rtx::CompositeQueue::setSettled`).
        std::optional<bool> mSettled;

        /// Which validation layers the run wants. Carried here and never in a settings file, for
        /// the reason `sValidationByDefault` gives.
        ValidationOptions mValidation;

        /// How long every frame stands for, in seconds, or nothing to time each one off the wall.
        /// Everything the world animates steps by it, so ten seconds of world is six hundred frames
        /// on every machine, and two runs of one build are the same run — which is what every run
        /// that measures or writes a picture wants, and the default. A window somebody watches
        /// wants the wall, as the played game has it, or the world runs as fast as the card draws.
        /// A run's and never a setting's: a file that could state a step once turned a played game
        /// into a fixed-step run for good.
        std::optional<float> mStep = sStepSeconds;
    };

    /// What a launcher reads back once `Engine::go` has returned.
    struct SessionResult
    {
        /// Non-zero where a hashed run differed from its reference, or where a stop could not be
        /// reached at all.
        int mExitStatus = 0;

        std::vector<BenchPlace> mPlaces;

        /// What the run printed, whole, for a launcher whose output is read rather than logged.
        std::string mReport;

        /// Where the run was left, as a stop that would put a camera back there, so a place somebody
        /// flew to and closed the window on is not lost. Nothing where no stop began.
        std::optional<Stop> mLeft;
    };

}
