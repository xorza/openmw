#include "views.hpp"

#include "parsefloat.hpp"
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
        /// A field written as a number, or a throw naming the view, the field and what was written.
        float parseNumber(const std::string& view, std::string_view field, const std::string& text)
        {
            const std::optional<float> value = parseFloat(text);
            if (!value.has_value())
                throw std::runtime_error(
                    "view \"" + view + "\" has " + std::string(field) + " \"" + text + "\", which is not a number");

            return *value;
        }

        float parseSpeed(const std::string& view, const std::string& text)
        {
            const float speed = parseNumber(view, "speed", text);
            if (!(speed > 0.0f))
                throw std::runtime_error("view \"" + view + "\" has speed \"" + text
                    + "\", which is not a positive number of units a second");

            return speed;
        }

        float parseHour(const std::string& view, const std::string& text)
        {
            const float hour = parseNumber(view, "hour", text);
            if (!(hour >= 0.0f) || !(hour < 24.0f))
                throw std::runtime_error("view \"" + view + "\" has hour \"" + text
                    + "\", which is not an hour of the day from 0 up to but not including 24");

            return hour;
        }

        /// Fills each borrower in from the view its `like` names.
        ///
        /// **A place at another hour is the same place, and this is what keeps it so.** A dawn row
        /// and a noon row of one camera mean something beside each other only where the camera is
        /// identical by construction; coordinates copied by hand drift the first time either is
        /// moved, and a pair measuring two cameras reads as a difference the hour made.
        ///
        /// **One level, and a route is not among what is taken.** The view a `like` names states its
        /// own place, which leaves no chain to walk and no cycle to detect. A route is left behind
        /// because flying from a place is a different measurement rather than the same place under
        /// another light, and a borrower that wants one writes its own.
        void resolveLikes(std::vector<View>& views, const std::vector<std::pair<std::size_t, std::string>>& likes)
        {
            for (const auto& [at, name] : likes)
            {
                View& borrower = views[at];
                if (borrower.mName == name)
                    throw std::runtime_error("view \"" + borrower.mName + "\" is like itself");

                const auto lent = std::find_if(
                    likes.begin(), likes.end(), [&](const auto& l) { return views[l.first].mName == name; });
                if (lent != likes.end())
                    throw std::runtime_error("view \"" + borrower.mName + "\" is like \"" + name
                        + "\", which is itself like another view; only a view that states its own place may be lent");

                const View* source = findView(views, name);
                if (source == nullptr)
                    throw std::runtime_error(
                        "view \"" + borrower.mName + "\" is like \"" + name + "\", which is not a view");

                // Written through the vector while `source` points into it, which the check above
                // makes safe: the two are different views and nothing here resizes.
                if (borrower.mCell.empty())
                    borrower.mCell = source->mCell;
                if (!borrower.mOrigin.has_value())
                    borrower.mOrigin = source->mOrigin;
                if (!borrower.mTarget.has_value())
                    borrower.mTarget = source->mTarget;
            }
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

    float hourFor(const std::optional<float>& given, const std::optional<float>& fixed)
    {
        return given.has_value() ? *given : fixed.value_or(sDefaultHour);
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
        std::vector<std::pair<std::size_t, std::string>> likes;

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
            else if (field == "hour")
                view.mHour = parseHour(section, value);
            else if (field == "like")
                likes.emplace_back(views.size() - 1, value);
            else
                throw std::runtime_error("view \"" + section + "\" has no field called \"" + field + "\"");
        }

        if (views.empty())
            throw std::runtime_error(Files::pathToUnicodeString(path) + " defines no views");

        // Before the cell is demanded and before a route is paired: a borrower takes both from what
        // it is like, and either check run first would reject a view that is about to be complete.
        resolveLikes(views, likes);

        for (const View& view : views)
            if (view.mCell.empty())
                throw std::runtime_error("view \"" + view.mName + "\" names no cell");

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

    std::vector<View> chooseViews(const std::vector<View>& views, const std::vector<std::string>& named)
    {
        // **"all" is a name nothing may take, and it means every view.** `bench` reaches this
        // through a suite as well, so the word has to mean the same on either road in.
        if (named.empty() || (named.size() == 1 && named.front() == "all"))
            return views;

        std::vector<View> chosen;
        chosen.reserve(named.size());
        for (const std::string& name : named)
        {
            const View* view = findView(views, name);
            if (view == nullptr)
                throw std::runtime_error("no view is called \"" + name + "\"; --list-views prints them");

            chosen.push_back(*view);
        }

        return chosen;
    }
}
