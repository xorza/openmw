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

        /// Nothing where the harness asked for no measured run, which is every played session.
        std::optional<Rtx::SessionRequest> mSession;

        /// Where the run's answer goes, wherever `mSession` is set. The caller's own, and it has to
        /// outlive `Engine::go`: the session fills it from its own destructor, which `~Engine` runs.
        Rtx::SessionResult* mInto = nullptr;
    };

    /// Drives a run of the game and measures it — the game, because a staged world never pays for
    /// the whole-graph walk, the sweep or a cell arriving, which are what cost a frame. It reads
    /// the world through `MWBase::Environment`, is fed each frame by `RtxRenderer`, and ends the
    /// run through `StateManager::requestQuit` the way the player's quit key does.
    class Session
    {
    public:
        Session(Rtx::SessionRequest request, Rtx::SessionResult& into);
        ~Session();

        bool isHeadless() const { return mRequest.mHeadless; }

        /// Which layers the run asked for.
        const Rtx::ValidationOptions& getValidation() const { return mRequest.mValidation; }

        /// How long every frame of the run stands for, or nothing for the wall: what the renderer's
        /// clock is made from.
        std::optional<float> getStep() const { return mRequest.mStep; }

        /// Whether this run states for itself that the ground waits, or nothing to let the frame
        /// clock decide. `Rtx::SessionRequest::mSettled` says which runs state one.
        std::optional<bool> getSettled() const { return mRequest.mSettled; }

        /// Which sample the trace should take, or nothing while no stop is running: the stop's own
        /// count and not the game's frame number, which carries every frame a loading screen drew
        /// and would put two runs of one binary at different points in the Halton sequence.
        std::optional<std::uint32_t> getSampleFrame() const;

        /// How many frames have gone into the running sum, this one included, or nought where the
        /// stop is not averaging. `Schedule::mAccumulate` says what that is for.
        std::uint32_t getAccumulated() const;

        /// Before the world is walked, because a teleport has to happen before the walk that would
        /// mirror the cell it left: starts the stop that is due, flies a route on, and turns a sky.
        /// Does nothing until the game has a world to stand in.
        void beforeFrame();

        /// Takes one traced frame the device answered for — `FrameReport::mResult` is set. Reports
        /// and asks the game to quit once the last stop is done.
        void frame(const FrameContext& context, const FrameReport& report);

        /// Whether the stop wants the graph walked a second time, so it can report what that added.
        bool wantsSecondWalk() const;

    private:
        /// Whether the game has a world with a player in it. Nothing happens before it does.
        bool isPlaying() const;

        /// Ends the run as a failure, saying why, and asks the game to quit. An exit status and not
        /// a throw, because a run that cannot go on has still measured whatever it reached.
        void abandon(std::string_view why);

        /// Gives the player every attribute and skill at 255, a Speed of 2000, level 255 and a
        /// million gold, through the calls the console's `setspeed`, `setlevel` and `additem` make.
        /// A body walking at Morrowind's pace crosses a cell in a minute.
        void boostPlayer();

        /// Puts the camera where the player stands, facing the way they face: what a stop that names
        /// no camera falls back to, and what a free-camera stop starts from.
        void standWhereThePlayerIs();

        /// Tells the renderer that nothing before this frame describes where it now stands.
        void forgetHistory();

        /// Puts the world where `mAt` says and starts counting.
        void beginStop();

        /// Closes the stop, records it, and moves to the next one — or ends the run. `report` is
        /// the last measured frame's, which is the frame every writer describes.
        void endStop(const FrameContext& context, const FrameReport& report);

        /// Writes what the run was asked to write and ends it.
        void finish();

        /// Takes note of where the eye stands, on a frame that still has a world to take it from:
        /// `OMW::Engine::~Engine` clears the world before the renderer that holds this, so the
        /// destructor can ask the world nothing.
        void noteStanding();

        /// Flies the player along the current stop's route by one frame's worth.
        void fly();

        /// Moves the sky one frame along the stop's list of weathers.
        void turnWeather();

        /// Puts the camera where the stop stands this frame, and points it where the stop asked.
        /// Every frame, because `omw/camera/camera.lua`'s `onActive` forces third person and a
        /// stop's teleport reactivates the player: aimed once, every view drew its frames from a
        /// camera 192 units behind the body. `Rtx::Check::CameraStands` says it still holds. A
        /// standing stop and a heading route are one case, carrying the heading forward from
        /// `mFlown`; nothing where the stop named no camera or gave it to the player.
        void aim();

        /// Points the game's camera along `look` from `eye`, for as long as nothing else moves it.
        void aimCamera(const osg::Vec3f& eye, const osg::Vec3f& look);

        /// What one stop has come to so far. `restart` rather than an assignment from a default,
        /// because the samples are reserved once for the longest stop of the run.
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

            /// Where the route has flown to: the route's own place and not the player's, because
            /// gravity steps the actor between frames and a step taken from where it landed compounds
            /// the fall.
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
            Rtx::Overlap mOverlap;
            Rtx::GpuClock mClock;

            /// Puts the route where it starts. One call, because two callers set the three and either
            /// could leave `mFlown` wherever the last stop's route ended.
            void standAt(const osg::Vec3f& eye, const osg::Vec3f& look)
            {
                mFrom = eye;
                mFromLook = look;
                mFlown = eye;
            }

            /// Empties it for the next stop, keeping the room every row grew.
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
                mOverlap = Rtx::Overlap{};
                mClock = Rtx::GpuClock{};
            }
        };

        Rtx::SessionRequest mRequest;

        /// Where the run's answer goes, written as this is destroyed.
        Rtx::SessionResult& mInto;

        /// Which stop is running, and whether it has been started.
        std::size_t mAt = 0;
        bool mStarted = false;

        /// Where the eye stood and what the sky was on the last frame the run drew, as a stop a
        /// launcher writes down. Empty until a stop has begun, and the run's rather than the stop's,
        /// because it answers where the run was left. The weather is assigned per frame into room
        /// the string already has.
        std::optional<Rtx::Stop> mStood;

        /// What the run has come to so far: the places, the report and the verdict. Its own type,
        /// because everything with something to say writes into all of it.
        Rtx::RunRecord mRecord;

        bool mDone = false;

        /// perf's control fifo, held for the whole run so every stop brackets its own frames.
        Rtx::PerfControl mProfiling;

        /// The card, watched across each stop's measured frames. Held rather than made per stop,
        /// because what it owns is a thread.
        Rtx::ClockWatch mClock;

        StopWriter mWriter;

        /// What a hashed frame lands in, refilled per measured frame and never freed. Not the
        /// writer's, which reads at a doll's or a tile's extent.
        std::vector<std::uint8_t> mPixels;

        /// Reserved once for the longest stop of the run, so the run itself does not allocate.
        StopProgress mProgress;
    };
}
