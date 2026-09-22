#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <optional>
#include <span>
#include <string_view>

namespace Rtx
{
    /// Where `name` sits in a menu that lists `entries` in order — the launcher's and the settings
    /// window's, which both list a `NamedEnum`'s spellings — or nothing for a name it does not
    /// offer.
    inline std::optional<std::size_t> menuIndex(
        const std::span<const std::string_view> entries, const std::string_view name)
    {
        const auto found = std::find(entries.begin(), entries.end(), name);
        if (found == entries.end())
            return std::nullopt;

        return static_cast<std::size_t>(std::distance(entries.begin(), found));
    }

    /// The entry at `index` of that menu, or nothing where the menu is shorter than that.
    inline std::optional<std::string_view> menuName(
        const std::span<const std::string_view> entries, const std::size_t index)
    {
        if (index >= entries.size())
            return std::nullopt;

        return entries[index];
    }

    /// One entry of a menu as an interface shows it: the setting's spelling, and the interface's
    /// own words for it — a MyGUI tag or a Qt source string, both of which the interface takes as
    /// a C string.
    struct MenuLabel
    {
        std::string_view mSpelling;
        const char* mLabel;
    };

    /// Whether `labels` names every entry of `menu`, in `menu`'s order. What an interface's table
    /// of labels is held to at compile time: the order is the core's, and a mode added there with
    /// no label, or a table in another order, stops the build rather than putting every label on
    /// its neighbour's mode. A table of another length does not deduce at all.
    template <std::size_t N>
    constexpr bool followsMenu(const std::array<MenuLabel, N>& labels, const std::array<std::string_view, N>& menu)
    {
        for (std::size_t at = 0; at < N; ++at)
            if (labels[at].mSpelling != menu[at])
                return false;

        return true;
    }
}
