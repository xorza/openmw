#include "verbs.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include <components/crashcatcher/crash.hpp>
#include <components/rtx/namedenum.hpp>

namespace RtxTool
{
    namespace
    {
        /// Every command and the word it is typed as, in the order `--help` prints them.
        ///
        /// **The one list of the names**, which is what makes an option's ownership and the
        /// dispatch's table the same statement: a command renamed here is renamed in both.
        constexpr Rtx::NamedEnum sNames{ std::array{
            std::pair{ Verbs::Info, std::string_view("info") },
            std::pair{ Verbs::Scene, std::string_view("scene") },
            std::pair{ Verbs::Shot, std::string_view("shot") },
            std::pair{ Verbs::View, std::string_view("view") },
            std::pair{ Verbs::Bench, std::string_view("bench") },
            std::pair{ Verbs::Check, std::string_view("check") },
            std::pair{ Verbs::Film, std::string_view("film") },
        } };
    }

    const VerbPolicy& policyOf(const Verbs one)
    {
        // In the order `sNames` names them. `view` flies nothing and freezes nothing, because
        // somebody is flying it; `check` and `shot` fly a route unfrozen and stand still elsewhere;
        // `bench` measures what moves.
        static constexpr std::array<std::pair<Verbs, VerbPolicy>, 7> sPolicies{ {
            { Verbs::Info, VerbPolicy{} },
            { Verbs::Scene, VerbPolicy{ .mFreezes = true } },
            { Verbs::Shot, VerbPolicy{ .mFreezes = true, .mFliesRoutes = true, .mHashes = true } },
            { Verbs::View, VerbPolicy{} },
            { Verbs::Bench, VerbPolicy{ .mFliesRoutes = true, .mMeasures = true } },
            { Verbs::Check, VerbPolicy{ .mFreezes = true, .mFliesRoutes = true } },
            { Verbs::Film, VerbPolicy{ .mFollowsTracks = true, .mMeasures = true } },
        } };

        for (const auto& [verb, policy] : sPolicies)
            if (verb == one)
                return policy;

        Crash::fatal("a command with no row in the policy table");
    }

    std::string_view verbName(const Verbs one)
    {
        return sNames.name(one);
    }

    Verbs verbNamed(const std::string_view name)
    {
        return sNames.named(name).value_or(Verbs::None);
    }

    std::string describeVerbs(const Verbs set)
    {
        std::size_t left = countVerbs(set);

        std::string result;
        for (const auto& [verb, name] : sNames.mNames)
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
