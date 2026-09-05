#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
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
        ///
        /// **Reachable by name and offered by no menu**, for the reason `sUpscaleMenu` gives.
        Off,

        /// A third of the output's width and height, so a ninth of its pixels — 1280×720 internal
        /// to 3840×2160. The fewest pixels the network will be handed for a given output.
        UltraPerformance,

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
            case Upscale::UltraPerformance:
                return "ultraperformance";
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

    /// The modes a menu offers, in the order it lists them: fewest pixels traced first, every pixel
    /// last, and each of them denoised.
    ///
    /// **Not every mode**, which is what the name says and the entries below leave out.
    ///
    /// **One list, because two menus offer it.** The launcher and the game's own settings window
    /// each turn a position in a list into a mode and back, and a list stated twice is two of them
    /// the moment a mode is added.
    ///
    /// **`Off` is not among them, and that is the whole of what the list decides.** Ray
    /// Reconstruction is this renderer's denoiser and not an upscaler bolted on to one, so turning
    /// it off does not trade sharpness for speed — it hands the frame to the wavelet filter instead
    /// and the picture is worse in every way. The mode stays reachable by name, for a reference
    /// render and for telling the two denoisers apart, and nobody is offered it in a menu.
    inline constexpr std::array sUpscaleMenu{ Upscale::UltraPerformance, Upscale::Performance, Upscale::Balanced,
        Upscale::Quality, Upscale::Dlaa };

    /// Where `mode` sits in that menu, or nothing for one it does not offer.
    inline std::optional<std::size_t> upscaleMenuIndex(Upscale mode)
    {
        const auto* found = std::find(sUpscaleMenu.begin(), sUpscaleMenu.end(), mode);
        if (found == sUpscaleMenu.end())
            return std::nullopt;

        return static_cast<std::size_t>(found - sUpscaleMenu.begin());
    }

    /// The mode at `index` of that menu, or nothing where the menu is shorter than that.
    ///
    /// **Asked rather than indexed, because the list of entries lives in a layout file.** A menu
    /// with an entry the list has no mode for would otherwise read past the end of it.
    inline std::optional<Upscale> upscaleAtMenu(std::size_t index)
    {
        if (index >= sUpscaleMenu.size())
            return std::nullopt;

        return sUpscaleMenu[index];
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
        if (name == "ultraperformance")
            return Upscale::UltraPerformance;
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

    /// Where the mode `name` spells sits in the menu — nothing where it spells no mode at all, and
    /// nothing where it spells one the menu does not offer.
    ///
    /// **Both menus ask exactly this**, of the same setting, and each answering it for itself was
    /// three lines of the same two questions in two files.
    inline std::optional<std::size_t> upscaleMenuIndex(std::string_view name)
    {
        if (const std::optional<Upscale> mode = upscaleNamed(name))
            return upscaleMenuIndex(*mode);

        return std::nullopt;
    }
}
