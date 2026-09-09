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
    /// One thing a run asserts about what the renderer was handed or what it drew.
    ///
    /// **A claim against the running game, where these used to be tests against a world of the
    /// harness's own.** That world read its cells by hand, dressed its people by rules of its own
    /// and derived its sky from the content files, so a claim proved there was a claim about a
    /// world nobody plays. What is here is the same claim asked of the world a player stands in.
    enum class Check
    {
        /// A second walk over the same graph adds no mesh and no material.
        ///
        /// **The property the incremental mirror rests on**, and the only way to ask it is to ask
        /// twice: the same crate met again has to resolve to the mesh already uploaded rather than
        /// to a copy of it.
        WalkTwice,

        /// Every placement wears a material something described.
        ///
        /// **A canary, and it should be nought.** A placement wearing nothing is a surface the
        /// content stated and the extractor could not read, which reaches the screen as grey.
        SurfacesDescribed,

        /// A room holds lights to cast.
        ///
        /// **Asked of an interior and answered yes by every exterior**, because a hillside at noon
        /// legitimately places none. A room with lamps in it that placed none is lit by its ambient
        /// alone, which is the failure that looks like a dark room rather than like a fault.
        LightsPlaced,

        /// A route crossed cell boundaries, and not every crossing had to rebuild.
        ///
        /// **The single most useful number a route produces.** An append builds the structures the
        /// ring brought; a rebuild builds every structure in the scene and re-describes the whole
        /// texture table, and the two are an order of magnitude apart.
        CrossingsAppend,

        /// An exterior's ground reaches past the square the simulation holds.
        ///
        /// **`Terrain::QuadTreeWorld` parents its chunks to nothing**, so distant land is the one
        /// thing a mirror cannot find by walking the graph — it is collected through a residency
        /// instead. A world whose scene stops at the active grid is one where that collection
        /// silently did nothing, and it looks like a world with a short view rather than like a
        /// fault.
        GroundReaches,

        /// No two lights stand at the same point.
        ///
        /// **The lamps of the cells the paging leaves dark are read out of the content files**,
        /// because `Terrain::pagedType` stands no `LIGH` and no walk of any graph can find one. A
        /// cell that then loads brings its own copy of every lamp, so the reach has to stop
        /// standing them — and a doubled lamp is twice the light with nothing to say so.
        LightsNotDoubled,

        /// Every texture the scene named could be read.
        ///
        /// **A canary, and it should be nought.** A texture the uploader could not read is drawn
        /// grey, which reads as a material fault rather than as a missing file.
        TexturesReadable,
    };

    /// What a check is called on a command line and in a report.
    std::string_view checkName(Check check);

    /// Every check there is, in the order they are run.
    std::span<const Check> everyCheck();

    /// Where a run was left: the camera, and the sky it stood under.
    ///
    /// **Reported and never asked for, which is what separates it from `Stand` below.** A stop
    /// names a cell and may leave the eye to the world; this is where the eye turned out to be, so
    /// every field is filled and none is a cell.
    ///
    /// **One type, because two things say it.** A run reports where it ended and a launcher writes
    /// that down as a place worth returning to, and the five numbers were spelled out in both — so
    /// a field added to the report reached the file only if somebody carried it across.
    ///
    /// **A look vector and not a rotation**, because that is what a view file states and what a
    /// camera is aimed with. `MWRender::Session` keeps the pose it samples every frame in a form of
    /// its own, for the reason its own member says.
    struct Standing
    {
        osg::Vec3f mEye;
        osg::Vec3f mLook;
        float mHour = 0.0f;

        /// Which day of Morrowind's own calendar, counted from the one a new game begins on. Only
        /// the moons read it.
        int mDay = 0;

        /// As the fallback settings spell it.
        std::string mWeather;
    };

    /// Where a stop stands: a place in the world, and where the eye is inside it.
    ///
    /// **A savegame restores what no pair of coordinates can** — the player, their equipment, the
    /// hour, the weather and every cell the run has already loaded — so a stop that names one
    /// stands exactly where the save left off. A cell and an eye is the other way to say it, and
    /// the one a view file can hold.
    struct Stand
    {
        /// The cell to teleport to, as Morrowind addresses one: a pair of integers is an
        /// exterior, anything else is an interior's name. Empty stays wherever the game already is.
        ///
        /// **The spelling and not an id, because the world is what turns one into the other.**
        /// `MWBase::World::findExteriorPosition` reads both forms, resolves the cell and fills in
        /// somewhere to stand — a `cocmarkerheading` where the content names one and the middle of
        /// the square where it does not. That is the answer `coc` gives a player, so a stop stands
        /// where a player typing the same word would.
        std::string mCell;

        /// Where the eye goes, and what it looks at. Both left out leaves the player where the
        /// cell put them and their own camera alone, which is what a run measuring an ordinary
        /// session wants.
        std::optional<osg::Vec3f> mEye;
        std::optional<osg::Vec3f> mLook;
    };

    /// What the sky does at a stop.
    ///
    /// **Named for the stop rather than for the sky**, because `Sky` is a namespace this one is
    /// inside the reach of: `Rtx` names `Sky::TimeOfDaySettings` and `Sky::SkyRoll`, and a type
    /// called `Sky` here would take the lookup from every one of them.
    ///
    /// **Asked of the game's own weather system rather than derived.** The harness used to work
    /// out a sun, an air and a set of moons from the content files at an hour it was told, which is
    /// a second answer to a question `MWWorld::WeatherManager` already answers — and the two
    /// disagreed about a transition, about a quasi-exterior's air, and about which weathers a
    /// region ever sees.
    struct StopSky
    {
        std::optional<float> mHour;

        /// Which day of Morrowind's own calendar, counted from the one a new game begins on.
        ///
        /// **Only the moons read it**, and they are the reason it is separate from the hour: a
        /// phase runs on a three-day cycle and a rise hour on a twenty-four day one, so no hour can
        /// stand for a date.
        std::optional<int> mDay;

        /// A weather as the content files spell it: `Clear`, `Overcast`, `Thunderstorm`. Set
        /// immediately, so a stop stands under it from its first frame.
        std::optional<std::string> mWeather;

        /// Weathers to turn the sky through while the stop runs, in order and round again.
        ///
        /// **A transition and not a switch**, because that is what the game does and what the
        /// renderer has to survive: the sky blends, and the precipitation of the weather arriving
        /// replaces the one leaving partway through — a whole emitter's meshes and textures freed
        /// on an ordinary frame, with no cell boundary anywhere near it.
        ///
        /// **Asking for it stops the run being a benchmark**: no two places stand under the same
        /// sky, so the rows are comparable with nothing.
        std::vector<std::string> mTurnThrough;
    };

    /// Where a stop flies to, and how fast.
    ///
    /// **A route is what puts a cell arriving into a measurement at all.** A camera standing still
    /// measures a frame; the cost worth seeing — the ring read off the disk, the models built, the
    /// sweep that follows the cells that left — only happens to a player who goes somewhere.
    struct Route
    {
        /// Where the eye ends and what it looks at there. Both left out flies forwards along
        /// whatever the stop was left facing, which is what a savegame's own heading gives.
        std::optional<osg::Vec3f> mTo;
        std::optional<osg::Vec3f> mLookTo;

        /// World units a second. A Morrowind exterior cell is 8,192 across, so this times the
        /// stop's length is roughly how many boundaries get crossed.
        float mSpeed = 0.0f;
    };

    /// How long a stop runs and what moves while it does.
    struct Schedule
    {
        /// How long it runs and how much of it is thrown away first. `BenchSpec`, which is the
        /// one spelling both the game and the harness read.
        BenchSpec mSpec;

        std::optional<Route> mRoute;

        /// How many differently-seeded frames to average into one picture, or nought for none.
        ///
        /// **A converged reference, which is the only ground truth a sampled renderer has.** One
        /// bounce per pixel estimates an integral without bias, so enough of them average to the
        /// value itself. Error falls as the square root of this, so four times the frames halves
        /// it: a hundred is a clean picture and a thousand is a reference.
        std::uint32_t mAccumulate = 0;

        /// Whether the world's clock is held still while the stop runs.
        ///
        /// **What a reference wants and a measurement does not.** A still frame traced many times
        /// is the same frame, which is what makes an accumulated picture converge and a repeat time
        /// the renderer rather than the animation. `DateTimeManager::setSimulationTimeScale` is
        /// where it lands, so nothing in the world moves — not an actor, not a plume, not the sea.
        bool mFrozen = false;

        /// Whether the player keeps their own camera and their own collision.
        ///
        /// **What a window is for.** A stop that pins a static camera at a view's coordinates is
        /// taking a picture; one that hands the body back is a session somebody flies, and a view
        /// file's coordinates are where a camera stands rather than where a body fits — so the
        /// walls come off with it.
        bool mFreeCamera = false;
    };

    /// What a stop writes, and when.
    struct Actions
    {
        /// Where the last measured frame is written as a PNG, or empty for none.
        std::filesystem::path mCapture;

        /// Where that frame's linear radiance goes, four floats a pixel, raw and at the render
        /// extent. **What a measurement is taken against**, where the PNG is what a picture is
        /// looked at as.
        std::filesystem::path mDump;

        /// Report the share of pixels whose accumulated bounce luminance passes each of a ladder
        /// of thresholds, beside the frame's other figures.
        ///
        /// **What a firefly is counted in, and the one thing bytes cannot say.** A bright bounce is
        /// scene-referred radiance and the display curve has spent that by the time a pixel is a
        /// byte, so the tail is read off the channel the accumulator wrote.
        bool mTail = false;

        /// Whether the scene the renderer was handed is reported: what it holds, what it could
        /// not place, and one number for the whole of it.
        bool mDigest = false;

        /// Whether the same graph is walked a second time, so what that added can be asked about.
        ///
        /// **A diagnostic and not a frame.** Nothing changes between the two walks, so every count
        /// for new geometry should be zero — which is the property the incremental mirror rests on,
        /// and the only way to ask it is to ask twice. It is the largest cost a frame has, so only
        /// a stop that asked pays for it.
        bool mWalkTwice = false;

        /// Where every texture the scene holds is written, vanilla beside de-lit, as one sheet.
        std::filesystem::path mSheet;

        /// Where one local-map tile of the place is written, framed the way the game's own compass
        /// frames one.
        std::filesystem::path mMapTile;

        /// Whose inventory doll to write, and where. Empty for a stop that draws none.
        ///
        /// **A picture of a subject and not of the world**, which is the half a frame never
        /// exercises: the body is assembled and dressed by `MWRender::NpcAnimation`, mirrored into
        /// a scene of its own and traced against it.
        std::string mDoll;
        std::filesystem::path mDollOut;

        /// A word to look for among the textures the world around this place is wearing.
        ///
        /// **Off the scene the renderer was handed**, which is what makes it useful: what it lists
        /// is what a frame of this place would actually trace, rather than what the content files
        /// say stands somewhere near.
        std::string mFind;

        /// Whether every measured frame is read back and hashed.
        ///
        /// **Asking for it stops the run being a benchmark**: a read back submits a copy and waits
        /// on it, so every frame is serialised against the device and the rows measure that.
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

    /// A whole run, as one description.
    ///
    /// **Filled by a launcher and read by the renderer, and neither knows the other.** The harness
    /// builds one out of a command line and a view file; the plain game builds one out of a single
    /// settings string. What each does to get here is its own business; what happens after is not.
    struct SessionRequest
    {
        std::vector<Stop> mStops;

        /// Whether the run keeps its window hidden while it happens.
        ///
        /// **Hidden costs a present per frame and nothing else**, so a headless run is not a
        /// different renderer — it is the same one with nobody watching. `view` is the one caller
        /// that asks for a window.
        bool mHeadless = true;

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
        /// frame clock decide — which is what every ordinary run does.
        ///
        /// **The one switch a measurement needs and a picture must not have.** A settled run is
        /// what makes two processes draw one picture, and it is also what puts
        /// `Rtx::CompositeQueue`'s whole bake on the frame that queued it: measured on
        /// `island-crossing`, 9.4 ms a frame of a main thread asleep, against 0.37 ms of hand-over
        /// the profile can see. So a run that means to time the streaming path says so here, and
        /// every run that compares a picture leaves it alone. `Rtx::CompositeQueue::setSettled`
        /// says what waiting is for.
        std::optional<bool> mSettled;

        /// Which validation layers the run wants.
        ///
        /// **Carried here and never in a settings file**, for the reason `sValidationByDefault`
        /// gives: a developer's diagnostic in a player's configuration is a build whose quoted
        /// numbers were measured through the layers because somebody left a line behind. A launcher
        /// states it on the command line for the one run it is making.
        ValidationOptions mValidation;
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

        /// Where the run was left.
        ///
        /// **What a window is for as much as the picture is.** Somebody flies to a place worth
        /// keeping and closes the window; without this the coordinates go with it, and the view
        /// file gains nothing. What is missing from it is the launcher's — a view id and a cell
        /// spelling are what it asked for in the first place.
        Standing mLeft;
    };

}
