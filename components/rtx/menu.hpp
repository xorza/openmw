#pragma once

#include <algorithm>
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

    /// The entry at `index` of that menu, or nothing where the menu is shorter than that — asked
    /// rather than indexed, because the list of entries lives in a layout file, and a menu with
    /// an entry the list has no mode for would otherwise read past the end of it.
    inline std::optional<std::string_view> menuName(
        const std::span<const std::string_view> entries, const std::size_t index)
    {
        if (index >= entries.size())
            return std::nullopt;

        return entries[index];
    }
}
