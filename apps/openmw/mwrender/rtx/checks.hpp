#pragma once

#include <string>

#include <components/rtxbench/benchrun.hpp>

#include "tracedrun.hpp"

namespace MWRender
{

    /// Whether one check holds of what `owner` was handed and what it drew, with what it found in
    /// `found` either way.
    ///
    /// @param crossings what the stop's route came to, which only that check reads.
    bool checkHolds(const TracedRun& run, Rtx::Check check, const Rtx::Crossings& crossings, std::string& found);
}
