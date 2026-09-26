#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

#include <apps/openmw/engine.hpp>
#include <apps/openmw/mwrender/rtx/framereport.hpp>
#include <apps/openmw/mwrender/rtx/rtxrun.hpp>

#include "cameradriver.hpp"
#include "measurer.hpp"
#include "model/benchrun.hpp"
#include "model/runrecord.hpp"
#include "stager.hpp"
#include "standingnote.hpp"
#include "stopwriter.hpp"

namespace MWRender
{
    class Renderer;
    struct RendererSpec;
}

namespace RtxTool
{
    /// Drives a run of the game and measures it — the game, because a staged world never pays for
    /// the whole-graph walk, the sweep or a cell arriving, which are what cost a frame. It reads
    /// the world through `MWBase::Environment`, is fed each frame by `MWRender::RtxRenderer`
    /// through the interface it implements, and ends the run through `StateManager::requestQuit`
    /// the way the player's quit key does.
    ///
    /// **The sequence and nothing else.** A stop is staged by the `Stager`, flown by the
    /// `CameraDriver`, noted by the `StandingNote` and measured by the `Measurer`; this is the
    /// engine's host that says which of them runs when, and the run's record they all write into.
    ///
    /// **The harness's, and built before the engine.** It makes the renderer with itself installed
    /// as `RtxSetup::mRun`, states the run's step and runs the schedule before each frame — and what
    /// it came to is read with `describe` once `Engine::go` has returned: the run that ends its last
    /// stop and the window somebody closes both end there, and only the first ever reaches `finish`.
    class Session final : public MWRender::RtxRun, public OMW::EngineHost
    {
    public:
        explicit Session(SessionRequest request);

        /// The renderer this tool exists to drive, whatever the user's settings file says, made
        /// with the run: the engine never reads `[RTX] enabled` and never sees the run.
        std::unique_ptr<MWRender::Renderer> createRenderer(const MWRender::RendererSpec& spec) override;

        /// The run's own step, or the wall for a window somebody watches.
        std::optional<float> getFrameStep() const override;

        std::optional<std::uint32_t> getSampleFrame() const override;
        std::uint32_t getAccumulated() const override;
        bool wantsSecondWalk() const override;
        bool wantsFrameCopy() const override;

        /// Starts the stop that is due, or moves the running one a frame on. Does nothing until the
        /// game has a world to stand in.
        void beforeFrame() override;

        /// Measures the frame, and asks the game to quit once the last stop is done.
        void frame(const MWRender::FrameContext& context, const MWRender::FrameReport& report) override;

        std::string_view describeTitle() override { return mNote.describeTitle(); }

        /// What the run came to: the places, the report, the verdict and where the eye was left.
        SessionResult describe() const;

    private:
        /// Whether the game has a world with a player in it. Nothing happens before it does.
        bool isPlaying() const;

        /// Ends the run as a failure, saying why, and asks the game to quit. An exit status and not
        /// a throw, because a run that cannot go on has still measured whatever it reached.
        void abandon(std::string_view why);

        /// Stages the stop `mAt` names and starts its camera, its note and its count.
        void beginStop();

        /// Closes the stop, records it, and moves to the next one — or ends the run. `report` is
        /// the last measured frame's, which is the frame every writer describes.
        void endStop(const MWRender::FrameContext& context, const MWRender::FrameReport& report);

        /// Writes what the run was asked to write and ends it.
        void finish();

        const Stop& currentStop() const { return mRequest.mStops[mAt]; }

        /// The warm-up of the running stop, in frames of the run's step.
        std::uint32_t currentWarmup() const;

        SessionRequest mRequest;

        /// What the renderer is made with: the request's setup, and this as the run. After the
        /// request, which it refers into.
        const MWRender::RtxSetup mInstalled;

        /// Which stop is running, and whether it has been started.
        std::size_t mAt = 0;
        bool mStarted = false;
        bool mDone = false;

        /// What the run has come to so far: the places, the report and the verdict. Its own type,
        /// because everything with something to say writes into all of it. Before the measurer,
        /// which writes into it.
        RunRecord mRecord;

        Stager mStager;
        CameraDriver mCamera;
        StandingNote mNote;
        Measurer mMeasurer;
        StopWriter mWriter;
    };
}
