#pragma once

#include <optional>
#include <string_view>

namespace Rtx
{
    /// What the trace does with the threads its launch handed it, before it resolves what they
    /// found.
    ///
    /// **One build of the shader for each of these, so that a measurement has something to be
    /// against.** The launch change and the forms of the reorder are separable only if every one of
    /// them can be run, and `.notes/rtx/ser-plan.md` §9 is what that run found: the launch is worth 6
    /// to 10 percent and the reorder costs 7 to 17 back, so `Off` is what the renderer takes. The
    /// `REORDER_*` values in `shaders/visibility.h` are the shader's side of these, and
    /// `VisibilityPass` is where the two are asserted to agree.
    enum class Reorder
    {
        /// The launch order the device chose, and nothing asked of it.
        Off,

        /// A hit object per primary ray and one reorder on it, sorted by the shader-table index the
        /// trace recorded and by where the hit is. Where the sources say to start.
        Hit,

        /// A coherence hint per primary ray and no hit object: the kind of shading ahead and two
        /// flags, and nothing about where the hit is. The one form that keeps the launch's own
        /// locality, which is what this trace's eleven channel writes are laid out along.
        Hint,

        /// Both of those as one key.
        Both,

        /// The hit object at the bounce ray instead of the eye's own. A primary ray is coherent to
        /// begin with and one diffuse bounce from it is not.
        Bounce,
    };

    /// How `reorder` is spelled on a command line. The other half of `reorderNamed`, and the one a
    /// report needs: a run is only comparable against another if what it says it did can be read
    /// back.
    inline std::string_view reorderName(Reorder reorder)
    {
        switch (reorder)
        {
            case Reorder::Off:
                return "off";
            case Reorder::Hit:
                return "hit";
            case Reorder::Hint:
                return "hint";
            case Reorder::Both:
                return "both";
            case Reorder::Bounce:
                return "bounce";
        }

        return "off";
    }

    /// The mode `name` spells, or nothing where it spells none of them. Nothing rather than a
    /// default, for the reason `upscaleNamed` gives.
    inline std::optional<Reorder> reorderNamed(std::string_view name)
    {
        if (name == "off")
            return Reorder::Off;
        if (name == "hit")
            return Reorder::Hit;
        if (name == "hint")
            return Reorder::Hint;
        if (name == "both")
            return Reorder::Both;
        if (name == "bounce")
            return Reorder::Bounce;

        return std::nullopt;
    }
}
