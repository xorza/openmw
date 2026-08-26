#pragma once

#include <optional>
#include <string_view>

namespace Rtx
{
    /// How the frame gets from the size it is traced at to the size it is shown at.
    ///
    /// **A quality level rather than a ratio**, because the ratio is the upscaler's to choose: what
    /// to render at for a given output is asked of it, and the answer has changed between versions
    /// of the network.
    ///
    /// Each backend reads this as whatever its platform offers — Ray Reconstruction on Vulkan. A
    /// build without one refuses anything but `Off` rather than quietly ignoring it.
    enum class Upscale
    {
        /// Trace and present at the same size, with no upscaler in the frame at all. What every test
        /// and every reference render uses, because a converged average is of the trace and not of
        /// a network's opinion of it.
        Off,

        /// Half the output's width and height, so a quarter of its pixels. What the frame budget is
        /// written against — 1920×1080 internal to 3840×2160.
        Performance,
        Balanced,
        Quality,

        /// **No upscaling, but still the upscaler**: render and output are the same size and it only
        /// denoises and antialiases. What separates the two halves of what it does, when a frame
        /// comes out softer than the reference and the question is which half softened it.
        Dlaa,
    };

    /// How `upscale` is spelled on a command line and in a setting file.
    ///
    /// The other half of `upscaleNamed`, and the one a report needs: a run is only comparable
    /// against another if what it says it did can be read back.
    inline std::string_view upscaleName(Upscale upscale)
    {
        switch (upscale)
        {
            case Upscale::Off:
                return "off";
            case Upscale::Performance:
                return "performance";
            case Upscale::Balanced:
                return "balanced";
            case Upscale::Quality:
                return "quality";
            case Upscale::Dlaa:
                return "dlaa";
        }

        return "off";
    }

    /// The mode `name` spells, or nothing where it spells none of them.
    ///
    /// **Nothing rather than a default.** A setting file and a command line both reach this, and
    /// silently rendering at a mode nobody asked for is how a typo becomes a performance measurement
    /// of the wrong thing.
    inline std::optional<Upscale> upscaleNamed(std::string_view name)
    {
        if (name == "off")
            return Upscale::Off;
        if (name == "performance")
            return Upscale::Performance;
        if (name == "balanced")
            return Upscale::Balanced;
        if (name == "quality")
            return Upscale::Quality;
        if (name == "dlaa")
            return Upscale::Dlaa;

        return std::nullopt;
    }
}
