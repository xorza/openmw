#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

#include <osg/Quat>
#include <osg/Vec3d>
#include <osg/Vec3f>

#include <components/rtx/reconstruction.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtxbench/benchrun.hpp>
#include <components/rtxbench/frametimes.hpp>
#include <components/rtxbench/runrecord.hpp>

#include "tracedrun.hpp"

namespace MWRender
{

    /// The run `[RTX] session` asks for, or nothing where nobody asked for one.
    ///
    /// **What lets the plain game measure itself.** A launcher installs a whole request; a played
    /// binary has only a settings file, so what it can say is how long the run is and how fast to
    /// fly — where it stands is the savegame's.
    std::optional<Rtx::SessionRequest> readSessionSetting();

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

        /// Whether this run states for itself that the ground waits, or nothing to let the frame
        /// clock decide. `Rtx::SessionRequest::mSettled` says which runs state one.
        std::optional<bool> getSettled() const { return mRequest.mSettled; }

        /// Which sample the trace should take, or nothing while no stop is running.
        ///
        /// **The stop's own count and not the game's frame number.** What the bounce sampler and
        /// the upscaler's jitter are walked by has to be the same sequence on every run, and a
        /// game's frame number carries every frame a loading screen happened to draw — measured,
        /// two runs of one binary then sat at different points in the Halton sequence and 47% of
        /// the frame differed by up to 38 of 255, however long the warm-up.
        ///
        /// **`apps/rtxtool/repeatable.sh` is where it shows**, and as a count rather than as a red
        /// run: what this walks reaches the picture and not the scene columns, and that gate reports
        /// a differing picture instead of failing on one. A sequence that came apart again would
        /// read there as pictures differing from an early frame on.
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
        /// the one `result.mWaitMs` cannot see. `spend` is what this fork owns of it, by phase, and
        /// `Rtx::Timing` says which of its figures is a share of which.
        void frame(const TracedRun& run, const Rtx::FrameResult& result, double frameMs, const Rtx::FrameSpend& spend,
            bool rebuilt);

        /// Whether the stop wants the graph walked a second time, so it can report what that added.
        bool wantsSecondWalk() const;

    private:
        /// Whether the game has a world with a player in it. Nothing happens before it does.
        bool isPlaying() const;

        /// Ends the run as a failure, saying why, and asks the game to quit.
        ///
        /// **The record's exit status and not a throw**, because a run that cannot go on has still
        /// measured whatever it reached: what it holds is reported, and the process leaves non-zero
        /// so nothing reads the report as a pass.
        void abandon(std::string_view why);

        /// Gives the player every attribute and skill at 255, a Speed of 2000, level 255 and a
        /// million gold, which is what a body somebody flies around a world wants.
        ///
        /// **Through the calls the console's `setspeed`, `setlevel` and `additem` make**, so the
        /// body stands where a player who typed them would. A session is for looking at the world,
        /// and a body walking at Morrowind's pace crosses a cell in a minute.
        void boostPlayer();

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
        void endStop(const TracedRun& run, const Rtx::Reconstruction& reconstruction);

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

        /// Puts the camera where the stop stands this frame, and points it where the stop asked.
        ///
        /// **Every frame, because the aim does not hold on its own.** `omw/camera/camera.lua` keeps
        /// out of `Mode::Static` in its `onFrame` and not in its `onActive`, which forces
        /// `MODE.ThirdPerson` outright — and a stop begins with a teleport, which reactivates the
        /// player and runs it. Aimed once, every standing view drew its first frame from the camera
        /// it named and every frame after that from a third-person camera 192 units behind the body
        /// at nought pitch and nought yaw — which is the frame `shot` writes and the frames `bench`
        /// measures. `Rtx::Check::CameraStands` is what says it still holds.
        ///
        /// **A standing stop and a heading route are one case.** A route moves `mFlown` and a
        /// standing stop leaves it where the stop began, so carrying the stop's own heading forward
        /// from `mFlown` is the aim for both. A route that names a destination is the one that aims
        /// at a point instead of along a heading.
        ///
        /// Nothing at all where the stop named no camera, or where a free-camera stop gave it to
        /// the player.
        void aim();

        /// Points the game's camera along `look` from `eye`, for as long as nothing else moves it.
        void aimCamera(const osg::Vec3f& eye, const osg::Vec3f& look);

        /// What one stop has come to so far.
        ///
        /// **One object, because `beginStop` resets it.** It was nine members of the session and
        /// four accumulators of `Held`, cleared by a ten-line block that a new field reaches only if
        /// its author remembers.
        ///
        /// **`restart` and not an assignment from a default**, because the samples are reserved once
        /// for the longest stop of the run: a bench that allocates where it measures is measuring
        /// its own allocation.
        struct StopProgress;

        Rtx::SessionRequest mRequest;

        /// Where the run's answer goes, or null where a settings file asked for the run and nobody
        /// is waiting on it. `RtxSetup::mInto` says what keeps it alive, and `~Session` says where a
        /// null one's report goes.
        Rtx::SessionResult* mInto = nullptr;

        /// Which stop is running, and whether it has been started.
        std::size_t mAt = 0;
        bool mStarted = false;

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
        ///
        /// **The run's and not the stop's**, because what it answers is where the run was left —
        /// which is a question asked after the last stop has closed.
        std::optional<Note> mStood;

        /// What the run has come to so far: the places, the report and the verdict. Its own type,
        /// because everything with something to say writes into all of it.
        Rtx::RunRecord mRecord;

        bool mDone = false;

        /// Out of line so this header names no container of samples, and reserved once so the run
        /// itself does not allocate — a bench that stutters where it measures is measuring its own
        /// stutter.
        ///
        /// **`Held` is what outlives a stop and `mProgress` is what does not**, which is the whole
        /// of why they are two objects.
        struct Held;
        std::unique_ptr<Held> mHeld;
        std::unique_ptr<StopProgress> mProgress;
    };
}
