#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>

#include "namedenum.hpp"

namespace Rtx
{
    /// How the frame gets from the size it is traced at to the size it is shown at — a quality
    /// level rather than a ratio, because the ratio is the upscaler's to choose. A build without an
    /// upscaler refuses anything but `Off`.
    enum class Upscale
    {
        /// Trace and present at the same size, with no upscaler in the frame at all — what every
        /// test and every reference render uses. Reachable by name and offered by no menu
        /// (`sUpscaleMenu`).
        Off,

        /// A third of the output's width and height, so a ninth of its pixels — 1280×720 internal
        /// to 3840×2160. The fewest pixels the network will be handed for a given output.
        UltraPerformance,

        /// Half the output's width and height, so a quarter of its pixels. What the frame budget is
        /// written against — 1920×1080 internal to 3840×2160.
        Performance,
        Balanced,
        Quality,

        /// No upscaling, but still the upscaler: render and output are the same size and it only
        /// denoises and antialiases. What separates the two halves of what it does, when a frame
        /// comes out softer than the reference and the question is which half softened it.
        Dlaa,
    };

    /// How an `Upscale` is spelled on a command line, in a setting file and in a report — the one
    /// list of the names, so a mode added here reaches the parser, the report and every line of
    /// prose that offers the modes at once.
    inline constexpr NamedEnum sUpscaleNames{ std::array{
        std::pair{ Upscale::Off, std::string_view("off") },
        std::pair{ Upscale::UltraPerformance, std::string_view("ultraperformance") },
        std::pair{ Upscale::Performance, std::string_view("performance") },
        std::pair{ Upscale::Balanced, std::string_view("balanced") },
        std::pair{ Upscale::Quality, std::string_view("quality") },
        std::pair{ Upscale::Dlaa, std::string_view("dlaa") },
    } };

    /// How `upscale` is spelled. The half a report needs: a run is only comparable against another
    /// if what it says it did can be read back.
    inline std::string_view upscaleName(Upscale upscale)
    {
        return sUpscaleNames.name(upscale);
    }

    /// The modes a menu offers, in the order it lists them: fewest pixels traced first, every pixel
    /// last, and each of them denoised. One list, because the launcher and the settings window both
    /// offer it. `Off` is not among them: Ray Reconstruction is this renderer's denoiser, so
    /// turning it off hands the frame to the wavelet filter and the picture is worse in every way.
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

    /// The mode at `index` of that menu, or nothing where the menu is shorter than that — asked
    /// rather than indexed, because the list of entries lives in a layout file, and a menu with an
    /// entry the list has no mode for would otherwise read past the end of it.
    inline std::optional<Upscale> upscaleAtMenu(std::size_t index)
    {
        if (index >= sUpscaleMenu.size())
            return std::nullopt;

        return sUpscaleMenu[index];
    }

    /// The mode `name` spells, or nothing where it spells none of them.
    inline std::optional<Upscale> upscaleNamed(std::string_view name)
    {
        return sUpscaleNames.named(name);
    }

    /// Where the mode `name` spells sits in the menu — nothing where it spells no mode at all, and
    /// nothing where it spells one the menu does not offer.
    inline std::optional<std::size_t> upscaleMenuIndex(std::string_view name)
    {
        if (const std::optional<Upscale> mode = upscaleNamed(name))
            return upscaleMenuIndex(*mode);

        return std::nullopt;
    }
}
