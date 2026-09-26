#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>

#include "namedenum.hpp"

namespace Rtx
{
    /// How the driver paces the frame, where the driver paces at all: off leaves its sleep a
    /// frame limiter and its markers a measurement; on asks for the low-latency mode, which is
    /// the driver holding the host until the frame's input can be sampled as late as it will still
    /// reach the screen on time; boost asks for the card's top clock beside it, whatever the frame
    /// would have let it idle at.
    enum class LatencyMode
    {
        Off,
        On,
        Boost,
    };

    /// How a `LatencyMode` is spelled in a setting file, on a command line and in a report — the
    /// one list, so a mode added here reaches the parser at once and stops the build until each
    /// menu gives it a label.
    inline constexpr NamedEnum sLatencyModeNames{ std::array{
        std::pair{ LatencyMode::Off, std::string_view("off") },
        std::pair{ LatencyMode::On, std::string_view("on") },
        std::pair{ LatencyMode::Boost, std::string_view("boost") },
    } };

    /// The modes the launcher and the settings window offer, in the order both list them: every
    /// one, so the menu is the spellings themselves.
    inline constexpr std::array<std::string_view, sLatencyModeNames.mNames.size()> sLatencyMenu
        = sLatencyModeNames.spellings();

    /// What a present paces the frame by, beside the vertical sync: the mode, and the shortest
    /// interval the driver holds two presents apart — the frame-rate limit, in microseconds,
    /// nought for none. The limit is here and not in a limiter of the host's own because the
    /// driver's sleep is the one place a frame may be held: held anywhere else, the frame's input
    /// is sampled that much earlier than it had to be.
    struct Pacing
    {
        LatencyMode mMode = LatencyMode::Off;
        std::uint32_t mMinimumIntervalUs = 0;

        bool operator==(const Pacing&) const = default;
    };

    /// The interval `[Video] framerate limit` asks for, in microseconds: a limit in frames a
    /// second, or nought or less for none. The setting takes any value above nought, and one
    /// under a frame in 71 minutes asks for more microseconds than the field holds, so the
    /// interval is held at the most it can say.
    inline constexpr std::uint32_t minimumIntervalOf(const float frameRateLimit)
    {
        if (!(frameRateLimit > 0.0f))
            return 0;

        const double interval = 1000000.0 / static_cast<double>(frameRateLimit) + 0.5;
        constexpr std::uint32_t most = std::numeric_limits<std::uint32_t>::max();
        return interval >= static_cast<double>(most) ? most : static_cast<std::uint32_t>(interval);
    }
}
