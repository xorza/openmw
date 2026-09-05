#include "verbs.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace RtxTool
{
    namespace
    {
        /// Every command and the word it is typed as, in the order `--help` prints them.
        ///
        /// **The one list of the names**, which is what makes an option's ownership and the
        /// dispatch's table the same statement: a command renamed here is renamed in both.
        constexpr std::array<std::pair<Verbs, std::string_view>, 9> sNames{
            std::pair{ Verbs::Info, std::string_view("info") },
            std::pair{ Verbs::Scene, std::string_view("scene") },
            std::pair{ Verbs::Shot, std::string_view("shot") },
            std::pair{ Verbs::View, std::string_view("view") },
            std::pair{ Verbs::Bench, std::string_view("bench") },
            std::pair{ Verbs::Textures, std::string_view("textures") },
            std::pair{ Verbs::Doll, std::string_view("doll") },
            std::pair{ Verbs::Map, std::string_view("map") },
            std::pair{ Verbs::Verify, std::string_view("verify") },
        };
    }

    std::string_view verbName(const Verbs one)
    {
        for (const auto& [verb, name] : sNames)
        {
            if (verb == one)
                return name;
        }

        return {};
    }

    Verbs verbNamed(const std::string_view name)
    {
        for (const auto& [verb, spelling] : sNames)
        {
            if (spelling == name)
                return verb;
        }

        return Verbs::None;
    }

    std::string describeVerbs(const Verbs set)
    {
        std::size_t left = 0;
        for (const auto& [verb, name] : sNames)
        {
            if (holds(set, verb))
                ++left;
        }

        std::string result;
        for (const auto& [verb, name] : sNames)
        {
            if (!holds(set, verb))
                continue;

            // The last of several is joined with "and", so a help line reads as a sentence rather
            // than as a list of flags.
            if (!result.empty())
                result += left == 1 ? " and " : ", ";

            result += '`';
            result += name;
            result += '`';
            --left;
        }

        return result;
    }
}
