#pragma once

#include <optional>
#include <string_view>

namespace RtxTool
{
    /// The number `text` spells, or nothing where it spells anything else.
    ///
    /// **The whole of the text, so a trailing letter is a refusal** rather than a number and a
    /// shrug: `speed = 1500u` is a typo, and a run that flew at 1500 anyway would report a figure
    /// nobody asked for. `Misc::StringUtils::toNumeric` answers the looser question and takes
    /// whatever prefix parses.
    ///
    /// **Read under the classic locale wherever this runs.** SDL asks the C library for the user's
    /// own on some platforms, and a decimal point read where a comma is the separator turns a view
    /// that flies at 1500 units a second into one that flies at one.
    std::optional<float> parseFloat(std::string_view text);
}
