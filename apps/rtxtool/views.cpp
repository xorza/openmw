#include "views.hpp"

#include "placement.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include <components/files/conversion.hpp>
#include <components/settings/categories.hpp>
#include <components/settings/parser.hpp>

namespace RtxTool
{
    namespace
    {
        float parseSpeed(const std::string& view, const std::string& text)
        {
            std::size_t read = 0;
            float speed = 0.0f;
            try
            {
                speed = std::stof(text, &read);
            }
            catch (const std::exception&)
            {
                read = 0;
            }

            if (read != text.size() || !(speed > 0.0f))
                throw std::runtime_error("view \"" + view + "\" has speed \"" + text
                    + "\", which is not a positive number of units a second");

            return speed;
        }

        /// Pairs each `to` with the view it names and with the `speed` beside it.
        ///
        /// **Both halves are required and neither has a default.** A route with no speed does not
        /// move and a speed with no destination has nowhere to go; either alone is a typo, and
        /// guessing what was meant is how a benchmark measures something other than what was asked
        /// for. The destination must also name its own `pos` and `look`, because a placement derived
        /// from a cell's bounds would need that cell staged to know it.
        void resolveRoutes(std::vector<View>& views, const std::vector<std::pair<std::size_t, std::string>>& ends,
            const std::vector<std::pair<std::size_t, float>>& speeds)
        {
            for (const auto& [at, speed] : speeds)
            {
                const auto paired
                    = std::find_if(ends.begin(), ends.end(), [&](const auto& e) { return e.first == at; });
                if (paired == ends.end())
                    throw std::runtime_error("view \"" + views[at].mName + "\" names a speed but nowhere to go");
            }

            for (const auto& [at, to] : ends)
            {
                const auto paired
                    = std::find_if(speeds.begin(), speeds.end(), [&](const auto& s) { return s.first == at; });
                if (paired == speeds.end())
                    throw std::runtime_error("view \"" + views[at].mName + "\" flies to \"" + to + "\" at no speed");

                const View* end = findView(views, to);
                if (end == nullptr)
                    throw std::runtime_error(
                        "view \"" + views[at].mName + "\" flies to \"" + to + "\", which is not a view");

                if (!end->mOrigin.has_value() || !end->mTarget.has_value())
                    throw std::runtime_error("view \"" + views[at].mName + "\" flies to \"" + to
                        + "\", which names no pos and look of its own to arrive at");

                views[at].mRoute = Route{
                    .mTo = to,
                    .mOrigin = *end->mOrigin,
                    .mTarget = *end->mTarget,
                    .mSpeed = paired->second,
                };
            }
        }
    }

    std::vector<View> loadViews(const std::filesystem::path& path)
    {
        Settings::CategorySettingValueMap entries;
        Settings::SettingsFileParser parser;

        // Reuses the settings file parser rather than growing a second one: the shape is the same,
        // a section per view and a key per field, and that parser already has tests.
        parser.loadSettingsFile(path, entries);

        // **Collected and resolved afterwards, because a route can point forwards.** The parser
        // hands sections back in the file's order and `to` may name a view that has not been read
        // yet, so the pairing waits until every section is in.
        std::vector<std::pair<std::size_t, std::string>> ends;
        std::vector<std::pair<std::size_t, float>> speeds;

        std::vector<View> views;
        for (const auto& [key, value] : entries)
        {
            const std::string& section = key.first;
            const std::string& field = key.second;

            if (views.empty() || views.back().mName != section)
                views.push_back(View{ .mName = section });

            View& view = views.back();
            if (field == "cell")
                view.mCell = value;
            else if (field == "pos")
                view.mOrigin = parseVec3(value, "pos");
            else if (field == "look")
                view.mTarget = parseVec3(value, "look");
            else if (field == "note")
                view.mNote = value;
            else if (field == "to")
                ends.emplace_back(views.size() - 1, value);
            else if (field == "speed")
                speeds.emplace_back(views.size() - 1, parseSpeed(section, value));
            else
                throw std::runtime_error("view \"" + section + "\" has no field called \"" + field + "\"");
        }

        for (const View& view : views)
            if (view.mCell.empty())
                throw std::runtime_error("view \"" + view.mName + "\" names no cell");

        if (views.empty())
            throw std::runtime_error(Files::pathToUnicodeString(path) + " defines no views");

        resolveRoutes(views, ends, speeds);
        return views;
    }

    float Route::partAt(const Placement& start, float seconds) const
    {
        const float length = (mOrigin - start.mOrigin).length();
        if (!(length > 0.0f))
            return 1.0f;

        return std::min(1.0f, std::max(0.0f, mSpeed * seconds / length));
    }

    Placement Route::at(const Placement& start, float part) const
    {
        return Placement{
            .mOrigin = start.mOrigin + (mOrigin - start.mOrigin) * part,
            .mTarget = start.mTarget + (mTarget - start.mTarget) * part,
        };
    }

    const View* findView(const std::vector<View>& views, std::string_view name)
    {
        const auto found = std::find_if(views.begin(), views.end(), [&](const View& v) { return v.mName == name; });
        return found == views.end() ? nullptr : &*found;
    }
}
