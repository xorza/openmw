#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <osg/Vec3f>

#include <components/rtx/renderer.hpp>
#include <components/rtxbench/benchrecord.hpp>
#include <components/rtxbench/benchspec.hpp>

namespace MWRender
{
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
    /// inside the reach of: `MWRender::SceneFrame` names `Sky::SkyRoll`, and a type of that name
    /// beside it would take the lookup.
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
        /// How long it runs and how much of it is thrown away first. `Rtx::BenchSpec`, which is the
        /// one spelling both the game and the harness read.
        Rtx::BenchSpec mSpec;

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

        /// Whether every measured frame is read back and hashed.
        ///
        /// **Asking for it stops the run being a benchmark**: a read back submits a copy and waits
        /// on it, so every frame is serialised against the device and the rows measure that.
        bool mHash = false;
    };

    /// One place a run visits, and everything that is true of it.
    struct Stop
    {
        /// What the report and the hashes call it. A view id where the run came from a view file.
        std::string mName;

        /// What the report prints beside the name.
        std::string mNote;

        /// The cell as `--cell` spells it, for the report only — where the run actually stands is
        /// `mStand`.
        std::string mCell;

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

        /// Whether the window is shown while the run happens.
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

        /// Which validation layers the run wants.
        ///
        /// **Carried here and never in a settings file**, for the reason `Rtx::sValidationByDefault`
        /// gives: a developer's diagnostic in a player's configuration is a build whose quoted
        /// numbers were measured through the layers because somebody left a line behind. A launcher
        /// states it on the command line for the one run it is making.
        Rtx::ValidationOptions mValidation;
    };

    /// What a launcher reads back once `Engine::go` has returned.
    struct SessionResult
    {
        /// Non-zero where a hashed run differed from its reference, or where a stop could not be
        /// reached at all.
        int mExitStatus = 0;

        std::vector<Rtx::BenchPlace> mPlaces;

        /// What the run printed, whole, for a launcher whose output is read rather than logged.
        std::string mReport;
    };

    /// The run `[RTX] session` asks for, or nothing where nobody asked for one.
    ///
    /// **What lets the plain game measure itself.** A launcher installs a whole request; a played
    /// binary has only a settings file, so what it can say is how long the run is and how fast to
    /// fly — where it stands is the savegame's.
    std::optional<SessionRequest> readSessionSetting();

    /// Hands a run to whichever renderer the engine is about to build.
    ///
    /// **A slot and not a field of `RendererSpec`.** That struct is filled inside `Engine::go`,
    /// which is upstream's; a field there would be an edit to it for a value only one launcher ever
    /// sets. Filled once before the engine starts and taken once by the renderer's constructor.
    void installSession(SessionRequest request);

    /// What was installed, or nothing for an ordinary session. Taken, so a second renderer in one
    /// process does not inherit the first one's run.
    std::optional<SessionRequest> takeInstalledSession();

    /// What the run came to, published by the session as it ends and taken by the launcher after
    /// `Engine::go` returns.
    void publishSessionResult(SessionResult result);
    SessionResult takeSessionResult();

    /// Drives a run of the game and measures it.
    ///
    /// **Why the game and not a world of the harness's own.** A staged world re-walks only its
    /// actors, so it never pays for the whole-graph walk, the sweep, or a cell arriving — the three
    /// things that actually cost a frame. Every renderer defect this fork found in the last stretch
    /// was invisible to a staged bench and obvious the moment the game was measured.
    ///
    /// **It changes nothing outside this directory.** It reads the world through
    /// `MWBase::Environment`, is fed each frame by `RtxRenderer`, and ends the run through
    /// `StateManager::requestQuit` the way the player's quit key does.
    class Session
    {
    public:
        explicit Session(SessionRequest request);
        ~Session();

        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;

        /// Whether the run wants a window shown.
        bool isHeadless() const { return mRequest.mHeadless; }

        /// Which layers the run asked for.
        const Rtx::ValidationOptions& getValidation() const { return mRequest.mValidation; }

        /// Which sample the trace should take, or nothing while no stop is running.
        ///
        /// **The stop's own count and not the game's frame number.** What the bounce sampler and
        /// the upscaler's jitter are walked by has to be the same sequence on every run, and a
        /// game's frame number carries every frame a loading screen happened to draw — measured,
        /// two runs of one binary then sat at different points in the Halton sequence and 47% of
        /// the frame differed by up to 38 of 255, however long the warm-up.
        std::optional<std::uint32_t> getSampleFrame() const;

        /// How many frames have gone into the running sum, this one included, or nought where the
        /// stop is not averaging. `Schedule::mAccumulate` says what that is for.
        std::uint32_t getAccumulated() const;

        /// Before the world is walked. Starts the stop that is due, flies a route on, and turns a
        /// sky. Does nothing until the game is running and has a world to stand in.
        ///
        /// **Here rather than after the frame, because a teleport has to happen before the walk
        /// that would mirror the cell it left.** A loading screen drives `renderGui` and never
        /// `renderFrame`, so nothing re-enters this.
        void beforeFrame();

        /// Takes one traced frame. Reports and asks the game to quit once the last stop is done.
        ///
        /// `frameMs` is the whole frame and not the wait: measured from one call to the next, so it
        /// carries everything the game does between them — which is the number a player feels and
        /// the one `result.mWaitMs` cannot see. `walkMs` and `placeMs` are the two shares of it
        /// this fork owns.
        void frame(Rtx::Renderer& renderer, const Rtx::FrameResult& result, double frameMs, double walkMs,
            double placeMs, bool rebuilt);

    private:
        /// Whether the game has a world with a player in it. Nothing happens before it does.
        bool isPlaying() const;

        /// Puts the world where `mAt` says and starts counting.
        void beginStop();

        /// Closes the stop, records it, and moves to the next one — or ends the run.
        void endStop(Rtx::Renderer& renderer);

        /// Writes what the run was asked to write and publishes the result.
        void finish();

        /// Flies the player along the current stop's route by one frame's worth.
        void fly();

        /// Moves the sky one frame along the stop's list of weathers.
        void turnWeather();

        /// Points the game's camera where the stop asked, and holds it there.
        void aimCamera(const osg::Vec3f& eye, const osg::Vec3f& look);

        SessionRequest mRequest;

        /// Which stop is running, and whether it has been started.
        std::size_t mAt = 0;
        bool mStarted = false;

        /// Frames seen since the stop began, warm-up included, and what the measured ones came to
        /// outside the distributions: how much of the last one hit something, and how long they
        /// took between them.
        std::uint32_t mSeen = 0;
        double mHitPercent = 0.0;
        double mWallMs = 0.0;

        /// Where the eye stood when the stop began, which a route flies from.
        osg::Vec3f mFrom;
        osg::Vec3f mFromLook;

        /// The cell the last flown frame was drawn in, so a change of it is a boundary crossed.
        /// Compared as an address and never read, which is all an identity needs.
        const void* mCell = nullptr;

        /// Which weather the turn is on, and how far into the transition to the next.
        std::size_t mTurnedTo = 0;
        float mTurned = 0.0f;

        /// What every stop has come to so far, and what they all stood under.
        std::vector<Rtx::BenchPlace> mPlaces;
        Rtx::BenchHeader mHeader;

        /// The report as it is built, so a launcher gets the whole of it rather than the log's
        /// timestamped halves.
        std::string mReport;

        int mExitStatus = 0;
        bool mDone = false;

        /// Out of line so this header names no container of samples, and reserved once so the run
        /// itself does not allocate — a bench that stutters where it measures is measuring its own
        /// stutter.
        struct Held;
        std::unique_ptr<Held> mHeld;
    };
}
