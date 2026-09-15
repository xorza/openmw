#pragma once

#include <array>
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
        /// (`Settings::RTXCategory::sUpscaleMenu`).
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
}
