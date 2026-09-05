#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace RtxTool
{
    /// Which of the harness's commands something is about, as a set of them.
    ///
    /// **A set and not one name, because that is the question the options ask.** An option is read
    /// by one command, or by two, or by every one of them, and the only place that fact was written
    /// down was the prose of its own help line — where nothing could check it. `--views` said
    /// `bench` there, and `shot --views=balmora` rendered Seyda Neen and reported it without a word.
    enum class Verbs : std::uint16_t
    {
        None = 0,

        Info = 1 << 0,
        Scene = 1 << 1,
        Shot = 1 << 2,
        View = 1 << 3,
        Bench = 1 << 4,
        Textures = 1 << 5,
        Doll = 1 << 6,
        Map = 1 << 7,
        Verify = 1 << 8,
        Check = 1 << 9,

        /// All ten, which is what an option nobody restricted is read by.
        Every = 0x3ff,
    };

    constexpr Verbs operator|(Verbs a, Verbs b)
    {
        return static_cast<Verbs>(static_cast<std::uint16_t>(a) | static_cast<std::uint16_t>(b));
    }

    /// Whether `one` is in `set`.
    constexpr bool holds(Verbs set, Verbs one)
    {
        return (static_cast<std::uint16_t>(set) & static_cast<std::uint16_t>(one)) != 0;
    }

    /// Every command that is not in `set`.
    constexpr Verbs otherThan(Verbs set)
    {
        return static_cast<Verbs>(~static_cast<std::uint16_t>(set) & static_cast<std::uint16_t>(Verbs::Every));
    }

    /// How many commands are in `set`.
    constexpr std::size_t countVerbs(Verbs set)
    {
        return static_cast<std::size_t>(std::popcount(static_cast<std::uint16_t>(set)));
    }

    /// What one command is typed as, or empty for a set that is not exactly one of them.
    std::string_view verbName(Verbs one);

    /// The command called `name`, or `Verbs::None` where nothing is called that.
    Verbs verbNamed(std::string_view name);

    /// The commands in `set`, spelled the way a help line spells them: "`bench` and `verify`".
    std::string describeVerbs(Verbs set);
}
