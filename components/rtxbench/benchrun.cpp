#include "benchrun.hpp"

#include <array>
#include <string_view>
#include <utility>

#include <components/rtx/namedenum.hpp>

namespace Rtx
{
    namespace
    {
        /// Every check and the word it is asked for by.
        ///
        /// **The one list of the names**, so a check renamed here is renamed in the command line
        /// and in the report at once, and a check added here reaches the runner without anybody
        /// remembering to list it a second time.
        constexpr Rtx::NamedEnum sChecks{ std::array{
            std::pair{ Check::WalkTwice, std::string_view("walk-twice") },
            std::pair{ Check::SurfacesDescribed, std::string_view("surfaces-described") },
            std::pair{ Check::LightsPlaced, std::string_view("lights-placed") },
            std::pair{ Check::GroundReaches, std::string_view("ground-reaches") },
            std::pair{ Check::LightsNotDoubled, std::string_view("lights-not-doubled") },
            std::pair{ Check::TexturesReadable, std::string_view("textures-readable") },
            std::pair{ Check::CrossingsAppend, std::string_view("crossings-append") },
        } };

        constexpr auto sEvery = sChecks.values();
    }

    std::string_view checkName(const Check check)
    {
        return sChecks.name(check);
    }

    std::span<const Check> everyCheck()
    {
        return sEvery;
    }
}
