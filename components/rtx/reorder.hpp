#pragma once

#include <array>
#include <optional>
#include <string_view>
#include <utility>

#include "namedenum.hpp"

namespace Rtx
{
    /// How the trace sorts the threads its launch handed it, between the traversal and the shader
    /// that resolves what they found.
    ///
    /// **One build of the shader for each of these, so that a measurement has something to be
    /// against.** The launch change and the forms of the reorder are separable only if every one of
    /// them can be run, and running them found the launch worth 6 to 10 percent against the dispatch
    /// it replaced and every form of the reorder 7 to 17 back, so `Off` is what the renderer takes. The
    /// `REORDER_*` values in `shaders/visibility.h` are the shader's side of these, and
    /// `VisibilityPass` is where the two are asserted to agree.
    enum class Reorder
    {
        /// The launch order the device chose, and nothing asked of it.
        Off,

        /// One reorder on the hit object the traversal answered, sorted by the shader it names and
        /// by where the hit is. Where the sources say to start.
        Hit,

        /// A coherence hint and not the hit object: two flags, and nothing about where the hit is.
        /// The one form that keeps the launch's own locality, which is what this trace's eleven
        /// channel writes are laid out along.
        Hint,

        /// Both of those as one key.
        Both,
    };

    /// How a `Reorder` is spelled on a command line, in a setting file and in a report. The one list
    /// of the names, for the reason `sUpscaleNames` gives.
    inline constexpr NamedEnum sReorderNames{ std::array{
        std::pair{ Reorder::Off, std::string_view("off") },
        std::pair{ Reorder::Hit, std::string_view("hit") },
        std::pair{ Reorder::Hint, std::string_view("hint") },
        std::pair{ Reorder::Both, std::string_view("both") },
    } };

    /// How `reorder` is spelled. The half a report needs: a run is only comparable against another
    /// if what it says it did can be read back.
    inline std::string_view reorderName(Reorder reorder)
    {
        return sReorderNames.name(reorder);
    }

    /// The mode `name` spells, or nothing where it spells none of them.
    inline std::optional<Reorder> reorderNamed(std::string_view name)
    {
        return sReorderNames.named(name);
    }
}
