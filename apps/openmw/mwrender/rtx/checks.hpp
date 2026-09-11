#pragma once

#include <string>

#include <components/rtxbench/benchrun.hpp>

#include "framereport.hpp"

namespace MWRender
{
    /// What a stop asked for and what it came to, beside the frame it drew.
    ///
    /// **Named for the reason `FrameContext` is.** Each of these is read by one claim and by nothing
    /// else, and each arrived as a parameter of its own through `StopWriter::write` and
    /// `StopWriter::runChecks` — neither of which reads either. A second was one parameter; a third
    /// would have been another.
    ///
    /// Borrowed and valid for one stop.
    struct StopFacts
    {
        /// What the stop's route came to, which only `CrossingsAppend` reads.
        const Rtx::Crossings& mCrossings;

        /// What the stop asked its camera to be, which only `CameraStands` reads.
        const Rtx::Stand& mStand;
    };

    /// Whether one check holds of what `run` was handed and what it drew, with what it found in
    /// `found` either way.
    bool checkHolds(const FrameContext& context, const FrameReport& report, Rtx::Check check, const StopFacts& facts,
        std::string& found);
}
