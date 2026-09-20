#pragma once

#include <cstdint>
#include <optional>

#include <components/rtxbench/runsetup.hpp>

namespace MWRender
{
    struct FrameContext;
    struct FrameReport;

    /// A run the harness drives through this renderer, as the renderer sees it per frame: what
    /// the run asks of the frame ahead and what it is fed once the frame is drawn. What a run
    /// decides before anything is built — the window, the layers, the clock — is data beside it in
    /// `RtxSetup`, and what it does to the world between frames is the engine host's
    /// (`OMW::EngineHost::beforeFrame`) and never this renderer's. The harness implements it and
    /// owns it, and reads the run's answer once `Engine::go` has returned. A played session is a
    /// run as well, `PlayedRun`, so the renderer never asks which host it is under.
    ///
    /// **An interface, because the run is the harness's and the renderer is the game's.** The run
    /// reads the world through `MWBase::Environment` and writes pictures, sheets and records that
    /// nothing a player does ever asks for, so it links into the harness alone; what the renderer
    /// needs of it fits in the questions below.
    class RtxRun
    {
    public:
        virtual ~RtxRun() = default;

        /// Which sample the trace should take, or nothing while no stop is running: the stop's own
        /// count and not the game's frame number, which carries every frame a loading screen drew
        /// and would put two runs of one binary at different points in the Halton sequence.
        virtual std::optional<std::uint32_t> getSampleFrame() const = 0;

        /// How many frames have gone into the running sum, this one included, or nought where the
        /// stop is not averaging. `Rtx::Schedule::mAccumulate` says what that is for.
        virtual std::uint32_t getAccumulated() const = 0;

        /// Whether the stop wants the graph walked a second time, so it can report what that added.
        virtual bool wantsSecondWalk() const = 0;

        /// Whether the frame should leave its picture for its report — `FrameOptions::mReadBack`,
        /// which a run that hashes every frame asks for and nothing a player does ever does.
        virtual bool wantsFrameCopy() const = 0;

        /// Takes one traced frame, and with it whatever the device answered for an earlier one —
        /// `FrameReport::mResult`, set where an answer came back this frame. Every traced frame and
        /// not only the answered ones, because an answer comes back a frame later or two by
        /// whether the card had finished when the frame after asked, and a run that counted
        /// answers stood at different points of its sequence in two runs of one build. The two
        /// halves meet by frame number, `FrameReport::mFrame` and `FrameResult::mFrame`.
        virtual void frame(const FrameContext& context, const FrameReport& report) = 0;
    };

    /// The run a played session is: every answer the played one, and nothing noted from any
    /// frame. The renderer holds one for a session that installed no run of its own.
    class PlayedRun final : public RtxRun
    {
    public:
        std::optional<std::uint32_t> getSampleFrame() const override { return std::nullopt; }
        std::uint32_t getAccumulated() const override { return 0; }
        bool wantsSecondWalk() const override { return false; }
        bool wantsFrameCopy() const override { return false; }
        void frame(const FrameContext& context, const FrameReport& report) override {}
    };

    /// What the harness installs before the engine starts, where the harness started this process:
    /// everything a run decides once, as data, and the run itself for what it answers per frame.
    /// Carried through `RendererSpec`, so who owns it is readable off the signature. A played
    /// session installs none, and `RtxRenderer` makes one from `[RTX]` and the played answers.
    struct RtxSetup
    {
        /// The knobs the run was made with, every one of them stated — the request's own.
        Rtx::RunSetup mSetup;

        /// The run this process drives. The harness's own, and it outlives the engine.
        RtxRun& mRun;
    };
}
