#pragma once

#include <string>

#include <components/rtxbench/benchrun.hpp>

namespace MWRender
{
    class RtxRenderer;

    /// Whether one check holds of what `owner` was handed and what it drew, with what it found in
    /// `found` either way.
    ///
    /// @param crossings what the stop's route came to, which only that check reads.
    bool checkHolds(RtxRenderer& owner, Rtx::Check check, const Rtx::Crossings& crossings, std::string& found);
}
