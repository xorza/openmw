#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "error.hpp"

namespace Rtx
{
    /// Whether `values` are an enum's first `N` values, each once and in any order: what a table
    /// covering an enum counted from nought has to hold.
    template <class Enum, std::size_t N>
    constexpr bool coversFromNought(const std::array<Enum, N>& values)
    {
        for (std::size_t value = 0; value < N; ++value)
        {
            std::size_t rows = 0;
            for (const Enum held : values)
                rows += static_cast<std::size_t>(held) == value ? 1 : 0;
            if (rows != 1)
                return false;
        }

        return true;
    }

    /// One enum's spellings, and the only place they are written, with the printable list derived
    /// from it: `--upscale` once offered five of the six modes it accepts.
    ///
    /// **Checked where it is declared**, which a constructor that only runs at compile time makes
    /// every table's declaration do: no value named twice, no spelling shared or empty, and for an
    /// enum with a `Count`, every value in enum order, because such an enum indexes something by
    /// the table. A table that breaks one does not compile, and the throw named below says which.
    template <class Enum, std::size_t N>
    struct NamedEnum
    {
        consteval NamedEnum(const std::array<std::pair<Enum, std::string_view>, N>& names)
            : mNames(names)
        {
            for (std::size_t at = 0; at < N; ++at)
            {
                if (mNames[at].second.empty())
                    throw "a NamedEnum spells a value as nothing";

                for (std::size_t other = at + 1; other < N; ++other)
                {
                    if (mNames[at].first == mNames[other].first)
                        throw "a NamedEnum names one value twice";
                    if (mNames[at].second == mNames[other].second)
                        throw "a NamedEnum gives two values one spelling";
                }
            }

            if constexpr (requires { Enum::Count; })
            {
                if (N != static_cast<std::size_t>(Enum::Count))
                    throw "a NamedEnum over an enum with a Count leaves a value out";
                for (std::size_t at = 0; at < N; ++at)
                    if (static_cast<std::size_t>(mNames[at].first) != at)
                        throw "a NamedEnum over an enum with a Count lists it out of order";
            }
        }

        std::array<std::pair<Enum, std::string_view>, N> mNames;

        /// How `value` is spelled, or empty for a value this does not name, because a name that
        /// is visibly missing beats one that is quietly wrong.
        constexpr std::string_view name(Enum value) const
        {
            for (const auto& [held, spelling] : mNames)
                if (held == value)
                    return spelling;

            return {};
        }

        /// The value `spelling` names, or nothing where it names none of them, because a typo
        /// silently running a default is a measurement of something else.
        constexpr std::optional<Enum> named(std::string_view spelling) const
        {
            for (const auto& [value, held] : mNames)
                if (held == spelling)
                    return value;

            return std::nullopt;
        }

        /// The value `spelling` names, refusing anything else with every spelling this does take,
        /// for both hosts.
        ///
        /// @param what the noun the message calls this, as "an upscale mode".
        Enum require(std::string_view spelling, std::string_view what) const
        {
            if (const std::optional<Enum> value = named(spelling))
                return *value;

            // Concatenated and not formatted: this header reaches the settings registry, which
            // every translation unit of the game includes, and `<format>` is not a price to pay
            // there for a cold path's message.
            throw InputError('"' + std::string(spelling) + "\" is not " + std::string(what) + ": " + list());
        }

        /// The values, in the order they are listed.
        constexpr std::array<Enum, N> values() const
        {
            std::array<Enum, N> result{};
            for (std::size_t at = 0; at < N; ++at)
                result[at] = mNames[at].first;

            return result;
        }

        /// The spellings, in the order they are listed: what a menu offering every value lists.
        constexpr std::array<std::string_view, N> spellings() const
        {
            std::array<std::string_view, N> result{};
            for (std::size_t at = 0; at < N; ++at)
                result[at] = mNames[at].second;

            return result;
        }

        /// Every spelling as one sentence — "off, hit, hint or both" — for a help line and for the
        /// error a name nobody knows is answered with.
        std::string list() const
        {
            std::string result;
            for (std::size_t at = 0; at < N; ++at)
            {
                if (at > 0)
                    result += at + 1 == N ? " or " : ", ";

                result += mNames[at].second;
            }

            return result;
        }
    };
}
