#pragma once

#include <cstdint>
#include <optional>

#include <components/rtx/reconstruction.hpp>
#include <components/rtx/renderer.hpp>

namespace MWRender
{
    struct FrameContext;
    struct FrameReport;

    /// A run the harness drives through this renderer, as the renderer sees it: what the run
    /// decides before anything is built — the window, the layers, the clock — and what it is fed
    /// on every frame. The harness implements it and owns it, and reads the run's answer once
    /// `Engine::go` has returned; a played session has none, and every question asked of one has
    /// the played answer written beside it where it is asked.
    ///
    /// **An interface, because the run is the harness's and the renderer is the game's.** The run
    /// reads the world through `MWBase::Environment` and writes pictures, sheets and records that
    /// nothing a player does ever asks for, so it links into the harness alone; what the renderer
    /// needs of it fits in the questions below.
    class RtxRun
    {
    public:
        virtual ~RtxRun() = default;

        /// Whether the window stays hidden, which saves a present per frame and nothing else.
        virtual bool isHeadless() const = 0;

        /// Which validation layers the run asked for.
        virtual const Rtx::ValidationOptions& getValidation() const = 0;

        /// How long every frame of the run stands for, or nothing for the wall: what the renderer's
        /// clock is made from.
        virtual std::optional<float> getStep() const = 0;

        /// Whether this run states for itself that the ground waits, or nothing to let the frame
        /// clock decide. `Rtx::SessionRequest::mSettled` says which runs state one.
        virtual std::optional<bool> getSettled() const = 0;

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

    /// What the harness installs before the engine starts, where the harness started this process.
    /// Carried through `RendererSpec`, so who owns it is readable off the signature; `GlRenderer`
    /// ignores it, which is why it hangs off the spec rather than sitting in it. A played session
    /// installs none, and reads its two knobs from `[RTX]`.
    struct RtxSetup
    {
        /// The knobs the run was made with, every one of them stated.
        Rtx::RenderProfile mProfile;

        /// The run this process drives. The harness's own, and it outlives the engine.
        RtxRun& mRun;
    };
}
