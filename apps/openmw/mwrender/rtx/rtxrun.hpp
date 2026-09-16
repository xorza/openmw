#pragma once

#include <cstdint>
#include <optional>

#include <components/rtx/reconstruction.hpp>
#include <components/rtx/renderer.hpp>

namespace MWRender
{
    struct FrameContext;
    struct FrameReport;

    /// A run the harness drives through this renderer, as the renderer sees it per frame: what
    /// the run asks of the frame ahead and what it is fed once the frame is drawn. What a run
    /// decides before anything is built — the window, the layers, the clock — is data beside it in
    /// `RtxSetup`. The harness implements it and owns it, and reads the run's answer once
    /// `Engine::go` has returned. A played session is a run as well, the one whose every answer
    /// is the played one, so the renderer never asks which host it is under.
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

        /// Before the world is walked, because a teleport has to happen before the walk that would
        /// mirror the cell it left.
        virtual void beforeFrame() = 0;

        /// Takes one traced frame the device answered for — `FrameReport::mResult` is set.
        virtual void frame(const FrameContext& context, const FrameReport& report) = 0;
    };

    /// What the harness installs before the engine starts, where the harness started this process:
    /// everything a run decides once, as data, and the run itself for what it answers per frame.
    /// Carried through `RendererSpec`, so who owns it is readable off the signature. A played
    /// session installs none, and `RtxRenderer` makes one from `[RTX]` and the played answers.
    struct RtxSetup
    {
        /// The knobs the run was made with, every one of them stated.
        Rtx::RenderProfile mProfile;

        /// Which validation layers the run asked for.
        Rtx::ValidationOptions mValidation;

        /// Whether the window stays hidden, which saves a present per frame and nothing else.
        bool mHeadless = false;

        /// Whether the trace counts what its rays hit. A report's figure — it is what tells "the
        /// cell rendered" from "the camera faced away from it" — and nothing a player does ever
        /// reads it, so a played session is specialized without the atomic rather than writing a
        /// number to a buffer nobody looks at, once per pixel that hit anything.
        bool mCountHits = false;

        /// How long every frame of the run stands for, or nothing for the wall: what the renderer's
        /// clock is made from.
        std::optional<float> mStep;

        /// Whether this run states for itself that the ground waits, or nothing to let the frame
        /// clock decide. `Rtx::SessionRequest::mSettled` says which runs state one.
        std::optional<bool> mSettled;

        /// The run this process drives. The harness's own, and it outlives the engine.
        RtxRun& mRun;
    };
}
