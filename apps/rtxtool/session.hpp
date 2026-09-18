#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include <osg/Vec3f>

#include <apps/openmw/mwrender/rtx/framereport.hpp>
#include <apps/openmw/mwrender/rtx/rtxrun.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtxbench/benchrecord.hpp>
#include <components/rtxbench/benchrun.hpp>
#include <components/rtxbench/frametimes.hpp>
#include <components/rtxbench/gpuclock.hpp>
#include <components/rtxbench/runrecord.hpp>
#include <components/rtxbench/scenedigest.hpp>

#include "stopwriter.hpp"

namespace MWBase
{
    class World;
}

namespace RtxTool
{
    /// Drives a run of the game and measures it — the game, because a staged world never pays for
    /// the whole-graph walk, the sweep or a cell arriving, which are what cost a frame. It reads
    /// the world through `MWBase::Environment`, is fed each frame by `MWRender::RtxRenderer`
    /// through the interface it implements, and ends the run through `StateManager::requestQuit`
    /// the way the player's quit key does.
    ///
    /// **The harness's, and built before the engine.** It is installed as `RtxSetup::mRun`, and
    /// what it came to is read with `describe` once `Engine::go` has returned — the run that ends
    /// its last stop and the window somebody closes both end there, and only the first ever
    /// reaches `finish`.
    class Session final : public MWRender::RtxRun
    {
    public:
        explicit Session(Rtx::SessionRequest request);

        std::optional<std::uint32_t> getSampleFrame() const override;
        std::uint32_t getAccumulated() const override;
        bool wantsSecondWalk() const override;
        bool wantsFrameCopy() const override;

        /// Starts the stop that is due, flies a route on, and turns a sky. Does nothing until the
        /// game has a world to stand in.
        void beforeFrame() override;

        /// Reports the frame, and asks the game to quit once the last stop is done.
        void frame(const MWRender::FrameContext& context, const MWRender::FrameReport& report) override;

        /// What the run came to: the places, the report, the verdict and where the eye was left.
        Rtx::SessionResult describe() const;

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
        void endStop(const MWRender::FrameContext& context, const MWRender::FrameReport& report);

        /// Takes in what the device answered for one frame, under the number of the frame it
        /// answers for: the figures a frame has only once the device is done with it, and its
        /// picture. Nothing for a frame the warm-up drew.
        void answered(const Rtx::FrameResult& finished, const Rtx::FrameExtents& extents);

        /// Hashes a frame's picture into the record, and writes it where the request asked for
        /// the pictures themselves, at `extents`.
        void keepPicture(const Rtx::FrameResult& finished, const Rtx::FrameExtents& extents);

        /// Writes what the run was asked to write and ends it.
        void finish();

        /// Takes note of where the eye stands, on a frame that still has a world to take it from:
        /// `OMW::Engine::~Engine` clears the world before the renderer that holds this, so the
        /// destructor can ask the world nothing.
        void noteStanding();

        /// Prints that note, whole, on the frame Home goes down: what a window prints where it
        /// was left, printed now, so a frame somebody is looking at can be drawn again without
        /// closing the window on it.
        void printStandingIfAsked();

        /// Flies the player along the current stop's route by one frame's worth.
        void fly();

        /// Moves the sky one frame along the stop's list of weathers.
        void turnWeather();

        /// Puts the sky under the weather called `name` over the player's region, as `changeweather`
        /// would, and warns for a name that is none of the ten.
        static void setWeather(MWBase::World& world, std::string_view name);

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
            /// Frames traced since the stop began, warm-up included. Traced and not answered for:
            /// `frame` says why the count is the run's and not the device's.
            std::uint32_t mSeen = 0;

            /// The backend's number of the first measured frame, so a result that comes back once
            /// the warm-up is over can say whether the frame it answers for was measured.
            std::uint64_t mFirstMeasured = 0;

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

            /// Whether the route has reached the destination it named. What ends a routed stop:
            /// the frames past arrival stand where the route ended and measure nothing the route
            /// was flown for, so `--seconds` is the ceiling a route that never arrives runs to.
            bool mArrived = false;

            /// Which weather the turn is on, and how far into the transition to the next.
            std::size_t mTurnedTo = 0;
            float mTurned = 0.0f;

            Rtx::FrameSamples mSamples;
            Rtx::GpuBreakdown mGpu;
            Rtx::Crossings mCrossings;
            Rtx::Overlap mOverlap;
            Rtx::HoldTimes mHold;
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
                mFirstMeasured = 0;
                mHitPercent = 0.0;
                mWallMs = 0.0;
                mCell = nullptr;
                mArrived = false;
                mTurnedTo = 0;
                mTurned = 0.0f;

                mSamples.clear();
                mGpu = Rtx::GpuBreakdown{};
                mCrossings = Rtx::Crossings{};
                mOverlap = Rtx::Overlap{};
                mHold = Rtx::HoldTimes{};
                mClock = Rtx::GpuClock{};
            }
        };

        Rtx::SessionRequest mRequest;

        /// Which stop is running, and whether it has been started.
        std::size_t mAt = 0;
        bool mStarted = false;

        /// Where the eye stood and what the sky was on the last frame the run drew, as a stop a
        /// launcher writes down. Empty until a stop has begun, and the run's rather than the stop's,
        /// because it answers where the run was left. The weather is assigned per frame into room
        /// the string already has.
        std::optional<Rtx::Stop> mStood;

        /// Whether Home was down on the last frame, so a press prints once.
        bool mPrintKeyHeld = false;

        /// What the run has come to so far: the places, the report and the verdict. Its own type,
        /// because everything with something to say writes into all of it.
        Rtx::RunRecord mRecord;

        bool mDone = false;

        /// perf's control fifo, held for the whole run so every stop brackets its own frames.
        Rtx::PerfControl mProfiling;

        /// The card, watched across each stop's measured frames. Held rather than made per stop,
        /// because what it owns is a thread.
        Rtx::ClockWatch mClockWatch;

        StopWriter mWriter;

        /// Reserved once for the longest stop of the run, so the run itself does not allocate.
        StopProgress mProgress;

        /// What a hashed frame's scene columns come from, kept so a frame pays for what moved.
        Rtx::SceneDigester mDigester;
    };
}
