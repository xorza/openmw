#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <osg/Node>
#include <osg/Vec3f>

#include <components/rtx/reconstruction.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtxbench/benchrecord.hpp>
#include <components/rtxbench/benchrun.hpp>
#include <components/rtxbench/frametimes.hpp>
#include <components/rtxbench/gpuclock.hpp>
#include <components/rtxbench/runrecord.hpp>

#include "framereport.hpp"
#include "stopwriter.hpp"

namespace MWRender
{
    /// What a harness run asks of the ray tracer, where a harness started this process. Carried
    /// through `RendererSpec`, so who owns the request and the result is readable off the
    /// signature; `GlRenderer` ignores it, which is why it hangs off the spec rather than sitting
    /// in it.
    struct RtxSetup
    {
        /// Nothing where the harness turned no knob, which is a run at whatever the settings say.
        std::optional<Rtx::RenderProfile> mProfile;

        /// Nothing where the harness asked for no measured run — `[RTX] session` is the other way
        /// in, and a played binary has only that one.
        std::optional<Rtx::SessionRequest> mSession;

        /// Where the run's answer goes. The caller's own, and it has to outlive `Engine::go`: the
        /// session fills it from its own destructor, which `~Engine` runs.
        Rtx::SessionResult* mInto = nullptr;
    };

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
        /// game's frame number carries every frame a loading screen happened to draw, so two runs
        /// of one binary sit at different points in the Halton sequence however long the warm-up.
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

        /// Takes one traced frame the device answered for — `FrameReport::mResult` is set. Reports
        /// and asks the game to quit once the last stop is done.
        void frame(const FrameContext& context, const FrameReport& report);

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

        /// Closes the stop, records it, and moves to the next one — or ends the run. `report` is
        /// the last measured frame's, which is the frame every writer describes.
        void endStop(const FrameContext& context, const FrameReport& report);

        /// Writes what the run was asked to write and ends it.
        void finish();

        /// Takes note of where the eye stands, on a frame that still has a world to take it from.
        ///
        /// **Because the destructor asks the world nothing.** `OMW::Engine::~Engine` clears its
        /// members in a body rather than leaving them to declaration order, and it clears the world
        /// and the state manager *before* the renderer that holds this — so every question put to
        /// `MWBase::Environment` from there goes through a pointer to something that has gone.
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

        /// What one stop has come to so far. One object, because `beginStop` resets it whole; and
        /// `restart` rather than an assignment from a default, because the samples are reserved
        /// once for the longest stop of the run and a bench that allocates where it measures is
        /// measuring its own allocation.
        struct StopProgress
        {
            /// Frames seen since the stop began, warm-up included.
            std::uint32_t mSeen = 0;

            /// What the measured ones came to outside the distributions: how much of the last one hit
            /// something, and how long they took between them.
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
            /// the fall forward and compounds it: a route flown six thousand units up ends along the
            /// ground and inside it.
            osg::Vec3f mFlown;

            /// The cell the last flown frame was drawn in, so a change of it is a boundary crossed.
            /// Compared as an address and never read, which is all an identity needs.
            const void* mCell = nullptr;

            /// Which weather the turn is on, and how far into the transition to the next.
            std::size_t mTurnedTo = 0;
            float mTurned = 0.0f;

            Rtx::FrameSamples mSamples;
            Rtx::GpuBreakdown mGpu;
            Rtx::Crossings mCrossings;
            Rtx::GpuClock mClock;

            /// Puts the route where it starts: where the eye stands, what it faces, and how far it has
            /// flown, which is nowhere yet.
            ///
            /// **One call, because the three are one fact and two callers set them.** A stop that names
            /// its own eye and one that falls back to the player's both land here, and either could
            /// have set the first two and left the third at wherever the last stop's route ended.
            void standAt(const osg::Vec3f& eye, const osg::Vec3f& look)
            {
                mFrom = eye;
                mFromLook = look;
                mFlown = eye;
            }

            /// Empties it for the next stop, keeping the room every row grew.
            ///
            /// **Cleared and not assigned over**, because `mSamples` is reserved once for the longest
            /// stop of the run and a bench that allocates where it measures is measuring itself.
            void restart()
            {
                mSeen = 0;
                mHitPercent = 0.0;
                mWallMs = 0.0;
                mCell = nullptr;
                mTurnedTo = 0;
                mTurned = 0.0f;

                mSamples.clear();
                mGpu = Rtx::GpuBreakdown{};
                mCrossings = Rtx::Crossings{};
                mClock = Rtx::GpuClock{};
            }
        };

        Rtx::SessionRequest mRequest;

        /// Where the run's answer goes, or null where a settings file asked for the run and nobody
        /// is waiting on it. `RtxSetup::mInto` says what keeps it alive, and `~Session` says where a
        /// null one's report goes.
        Rtx::SessionResult* mInto = nullptr;

        /// Which stop is running, and whether it has been started.
        std::size_t mAt = 0;
        bool mStarted = false;

        /// Where the eye stood and what the sky was on the last frame the run drew, as a launcher
        /// reads it back. Empty until a stop has begun, so a run that reached no place describes
        /// none rather than describing wherever the new game happened to start.
        ///
        /// **The run's and not the stop's**, because what it answers is where the run was left —
        /// which is a question asked after the last stop has closed.
        ///
        /// **Kept as a stop, which is what a launcher writes down**, with the one conversion — a
        /// facing to a point looked at, a weather id to its name — made where the note is taken.
        /// The weather is a `std::string` assigned per frame, which is a copy into room the string
        /// already has; the name, the note and the cell are the running stop's, copied once as it
        /// begins.
        std::optional<Rtx::Stop> mStood;

        /// What the run has come to so far: the places, the report and the verdict. Its own type,
        /// because everything with something to say writes into all of it.
        Rtx::RunRecord mRecord;

        bool mDone = false;

        /// perf's control fifo, held for the whole run so every stop brackets its own frames.
        Rtx::PerfControl mProfiling;

        /// The card, watched across each stop's measured frames. **Held rather than made per stop**,
        /// because what it owns is a thread: one that is started and stopped by every stop is a
        /// thread made and joined at every place of a suite.
        Rtx::ClockWatch mClock;

        StopWriter mWriter;

        /// What a hashed frame lands in, refilled per measured frame and never freed.
        ///
        /// **Not shared with the writer's**, which reads at a doll's or a tile's extent rather than
        /// the frame's — one buffer would grow to the largest of them and stay there.
        std::vector<std::uint8_t> mPixels;

        /// Reserved once for the longest stop of the run, so the run itself does not allocate — a
        /// bench that stutters where it measures is measuring its own stutter.
        StopProgress mProgress;
    };
}
