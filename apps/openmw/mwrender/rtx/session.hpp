#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include <osg/Quat>
#include <osg/Vec3d>
#include <osg/Vec3f>

#include <components/rtx/reconstruction.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtxbench/benchrun.hpp>
#include <components/rtxbench/runrecord.hpp>

namespace MWRender
{
    class RtxRenderer;

    /// The run `[RTX] session` asks for, or nothing where nobody asked for one.
    ///
    /// **What lets the plain game measure itself.** A launcher installs a whole request; a played
    /// binary has only a settings file, so what it can say is how long the run is and how fast to
    /// fly — where it stands is the savegame's.
    std::optional<Rtx::SessionRequest> readSessionSetting();

    /// A run to make, and where to leave what it came to.
    struct InstalledSession
    {
        Rtx::SessionRequest mRequest;

        /// The launcher's own slot, filled once by `~Session` and never read here.
        ///
        /// **Null for a run a settings file asked for**, which is a played binary measuring itself
        /// with nobody waiting on the answer. That run's report goes to the log instead.
        Rtx::SessionResult* mInto = nullptr;
    };

    /// Hands a run to whichever renderer the engine is about to build, and says where its answer
    /// goes.
    ///
    /// **A slot and not a field of `RendererSpec`.** That struct is filled inside `Engine::go`,
    /// which is upstream's; a field there would be an edit to it for a value only one launcher ever
    /// sets. Filled once before the engine starts and taken once by the renderer's constructor.
    ///
    /// **`into` is the caller's own and has to outlive `Engine::go`.** The session fills it from
    /// its own destructor, which `~Engine` runs, so by the time `go` returns there is nothing left
    /// to ask — an answer read afterwards is one that was written somewhere first.
    void installSession(Rtx::SessionRequest request, Rtx::SessionResult& into);

    /// What was installed, or nothing for an ordinary session. Taken, so a second renderer in one
    /// process does not inherit the first one's run.
    std::optional<InstalledSession> takeInstalledSession();

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
        Session(Rtx::SessionRequest request, Rtx::SessionResult* into);
        ~Session();

        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;

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
        void frame(RtxRenderer& owner, const Rtx::FrameResult& result, double frameMs, double walkMs, double placeMs,
            bool rebuilt);

        /// Whether the stop wants the graph walked a second time, so it can report what that added.
        bool wantsSecondWalk() const;

    private:
        /// Whether the game has a world with a player in it. Nothing happens before it does.
        bool isPlaying() const;

        /// Puts the camera where the player stands, facing the way they face.
        ///
        /// **What a stop that names no camera of its own falls back to**, and what a free-camera
        /// stop starts from once the walls are down.
        void standWhereThePlayerIs();

        /// Tells the renderer that nothing before this frame describes where it now stands.
        ///
        /// **Named once because two callers mean it for two reasons**, and because the reach to say
        /// it is four hops through the game.
        void forgetHistory();

        /// Puts the world where `mAt` says and starts counting.
        void beginStop();

        /// Closes the stop, records it, and moves to the next one — or ends the run.
        ///
        /// @param reconstruction what put the last measured frame back together, which is the frame
        ///        every writer describes.
        void endStop(RtxRenderer& owner, const Rtx::Reconstruction& reconstruction);

        /// Writes what the run was asked to write and ends it.
        void finish();

        /// Everything a launcher reads back, including where the eye was left.
        ///
        /// **Written into `mInto` from the destructor and nowhere else.** A run that ends its last
        /// stop and a window somebody closes both come here, and only one of the two ever reaches
        /// `finish`.
        ///
        /// **The whole result and never a field at a time.** A launcher reads four of these and a
        /// run fills all four, so a hand-over written out member by member loses whichever ones
        /// nobody remembered — silently, since an unfilled `Rtx::SessionResult` is a valid one
        /// describing a camera at the origin.
        ///
        /// **And it asks the world nothing.** `OMW::Engine::~Engine` clears its members in a body
        /// rather than leaving them to declaration order, and it clears the world and the state
        /// manager *before* the renderer that holds this — so every question put to
        /// `MWBase::Environment` from here goes through a pointer to something that has gone.
        /// Where the run was left is read off the note `noteStanding` took instead.
        Rtx::SessionResult describeRun() const;

        /// Takes that note, on a frame that still has a world to take it from.
        void noteStanding();

        /// Flies the player along the current stop's route by one frame's worth.
        void fly();

        /// Moves the sky one frame along the stop's list of weathers.
        void turnWeather();

        /// Points the game's camera where the stop asked, and holds it there.
        void aimCamera(const osg::Vec3f& eye, const osg::Vec3f& look);

        Rtx::SessionRequest mRequest;

        /// Where the run's answer goes, or null where nobody installed a run. `installSession`
        /// says what keeps it alive, and `~Session` says where a null one's report goes.
        Rtx::SessionResult* mInto = nullptr;

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

        /// Where the route has flown to, which is not where the player stands.
        ///
        /// **The route's own place, because deriving the next step from the player puts physics in
        /// it.** `moveObjectBy` moves an actor, and the world then steps that actor: gravity pulls
        /// it down between one frame and the next, and a step taken from where it landed carries
        /// the fall forward and compounds it. `island-crossing` asks to be flown six thousand units
        /// up and was flown at eighty-five to twelve hundred — along the ground and inside it — so
        /// every number ever taken over it described a view nobody asked for.
        osg::Vec3f mFlown;

        /// The cell the last flown frame was drawn in, so a change of it is a boundary crossed.
        /// Compared as an address and never read, which is all an identity needs.
        const void* mCell = nullptr;

        /// Where the eye stood, what it faced and what the sky was, on one frame.
        ///
        /// **The numbers and never the names**, because `noteStanding` writes one of these every
        /// frame and a weather's spelling is a `std::string`. `describeRun` turns the last one into
        /// the `Rtx::Standing` a launcher reads, and it is reached once.
        struct Note
        {
            osg::Vec3d mAt;
            osg::Quat mFacing;
            float mHour = 0.0f;
            int mDay = 0;
            int mWeather = 0;
        };

        /// The last frame the run drew. Empty until a stop has begun, so a run that reached no
        /// place describes none rather than describing wherever the new game happened to start.
        std::optional<Note> mStood;

        /// Which weather the turn is on, and how far into the transition to the next.
        std::size_t mTurnedTo = 0;
        float mTurned = 0.0f;

        /// What the run has come to so far: the places, the report and the verdict. Its own type,
        /// because everything with something to say writes into all of it.
        Rtx::RunRecord mRecord;

        bool mDone = false;

        /// Out of line so this header names no container of samples, and reserved once so the run
        /// itself does not allocate — a bench that stutters where it measures is measuring its own
        /// stutter.
        struct Held;
        std::unique_ptr<Held> mHeld;
    };
}
