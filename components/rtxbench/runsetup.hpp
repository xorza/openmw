#pragma once

#include <cstdint>
#include <optional>

#include <components/rtx/cellworld.hpp>
#include <components/rtx/pacing.hpp>
#include <components/rtx/reconstruction.hpp>
#include <components/rtx/renderer.hpp>

#include "benchspec.hpp"

namespace Rtx
{
    /// Everything a run decides once, before anything is built, that the game's renderer reads:
    /// the harness fills one from its command line and a played session from `[RTX]`. One struct
    /// and not a field list copied from the request to the renderer, so a knob added here reaches
    /// both hosts by being here.
    struct RunSetup
    {
        /// The knobs the frames are traced under, every one of them stated.
        RenderProfile mProfile;

        /// Which validation layers the run asked for. Carried here and never in a settings file,
        /// for the reason `sValidationByDefault` gives.
        ValidationOptions mValidation;

        /// How much world the mirror builds and what of it, as the run decided: the harness from
        /// its command line, a played session from `[RTX] distant land cells` and the paging's
        /// two. Here and not written into the registry by the harness, because the registry is the
        /// player's and a knob of a run travels with the run.
        MirrorKnobs mMirror;

        /// How the driver paces the frame, as the run decided: the harness off for a measured run,
        /// where the driver's sleep is a limiter with no limit and its markers a measurement, and
        /// the player's for a watched window and a played session. Here and not written into the
        /// registry by the harness, for the reason `mMirror` gives. The frame-rate limit is not
        /// here: it is the engine's, which hands it to whichever renderer it made.
        LatencyMode mLatency = LatencyMode::Off;

        /// Whether the window stays hidden, which saves a present per frame and nothing else.
        bool mHeadless = false;

        /// How long every frame stands for, in seconds, or nothing to time each one off the wall.
        /// Everything the world animates steps by it, so ten seconds of world is six hundred frames
        /// on every machine, and two runs of one build are the same run — which is what every run
        /// that measures or writes a picture wants. A window somebody watches wants the wall, as
        /// the played game has it, or the world runs as fast as the card draws. A run's and never
        /// a setting's: a file that could state a step once turned a played game into a
        /// fixed-step run for good.
        std::optional<float> mStep;

        /// What one frame of world counts for where a run turns seconds into frames — a span, a
        /// flight, a turning sky: the stated step, or where the wall decides, the step a measured
        /// run states by default.
        float getWorldStep() const { return mStep.value_or(sStepSeconds); }

        /// Whether each walk waits for the cell it adopts, or nothing to let the frame clock
        /// decide. Settled is what makes two processes draw one picture; a run timing the
        /// streaming path says no (`Rtx::CellRing::setSettled`).
        std::optional<bool> mSettled;

        /// `RendererOptions::mMemoryBudget`: the harness's, for a run that asks what a smaller
        /// card does, and never a played session's.
        std::optional<std::uint64_t> mMemoryBudget;

        /// Whether the renderer reads its shaders with their source in them, for a profiler that
        /// shows a shader's lines (`Rtx::shaderDirectory`). The harness's, and never a played
        /// session's: the driver's cache is keyed on the modules without it.
        bool mShaderSource = false;
    };
}
