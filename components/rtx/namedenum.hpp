#pragma once

#include <array>
#include <cstddef>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace Rtx
{
    /// One enum's spellings, and the only place they are written, with the printable list derived
    /// from it: `--upscale` once offered five of the six modes it accepts.
    template <class Enum, std::size_t N>
    struct NamedEnum
    {
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

            throw std::runtime_error(std::format("\"{}\" is not {}: {}", spelling, what, list()));
        }

        /// The values, in the order they are listed.
        constexpr std::array<Enum, N> values() const
        {
            std::array<Enum, N> result{};
            for (std::size_t at = 0; at < N; ++at)
                result[at] = mNames[at].first;

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
