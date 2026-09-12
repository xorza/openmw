#include "run.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <stdexcept>
#include <utility>

#include <osg/Math>

#include <components/files/conversion.hpp>
#include <components/rtx/skylight.hpp>
#include <components/rtxbench/benchrecord.hpp>
#include <components/rtxbench/benchspec.hpp>
#include <components/settings/categories.hpp>
#include <components/settings/parser.hpp>

#include "options.hpp"

namespace RtxTool
{
    namespace
    {
        /// A view id derived from a cell's name, for a window that was opened without one.
        ///
        /// Something to paste rather than something to keep: the ids in the file are chosen to say
        /// what a view is *for*, which a cell name cannot.
        std::string slugOf(std::string_view cell)
        {
            std::string slug;
            for (const char letter : cell)
            {
                const bool plain = (letter >= 'a' && letter <= 'z') || (letter >= 'A' && letter <= 'Z')
                    || (letter >= '0' && letter <= '9');
                if (plain)
                    slug += static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
                else if (!slug.empty() && slug.back() != '-')
                    slug += '-';
            }

            while (!slug.empty() && slug.back() == '-')
                slug.pop_back();

            return slug.empty() ? "new-view" : slug;
        }

    }

    float Viewpoint::getBearing() const
    {
        osg::Vec3f forward = mAt.mLook - mAt.mEye;
        forward.normalize();

        const float degrees = osg::RadiansToDegrees(std::atan2(forward.x(), forward.y()));
        return degrees < 0.0f ? degrees + 360.0f : degrees;
    }

    float Viewpoint::getClimb() const
    {
        osg::Vec3f forward = mAt.mLook - mAt.mEye;
        forward.normalize();

        // Clamped because a normalised vector's z can land a bit past one, and `asin` answers a NaN
        // rather than ninety degrees when it does.
        return osg::RadiansToDegrees(std::asin(std::clamp(forward.z(), -1.0f, 1.0f)));
    }

    std::string describeSpot(const Viewpoint& spot)
    {
        return std::format("# {} at {:.0f}, {:.0f}, {:.0f} — bearing {:.0f}°, climb {:.0f}° — day {}, {}, {}\n",
            spot.mCell, spot.mAt.mEye.x(), spot.mAt.mEye.y(), spot.mAt.mEye.z(), spot.getBearing(), spot.getClimb(),
            spot.mAt.mDay, Rtx::describeHour(spot.mAt.mHour), spot.mAt.mWeather);
    }

    std::string describeBlock(const Viewpoint& spot)
    {
        std::string block = std::format("[{}]\n", spot.mView.empty() ? slugOf(spot.mCell) : spot.mView);

        if (!spot.mNote.empty())
            block += std::format("note = {}\n", spot.mNote);

        block += std::format("cell = {}\npos = {}, {}, {}\nlook = {}, {}, {}\n", spot.mCell, spot.mAt.mEye.x(),
            spot.mAt.mEye.y(), spot.mAt.mEye.z(), spot.mAt.mLook.x(), spot.mAt.mLook.y(), spot.mAt.mLook.z());

        // **Each condition only where the window was not at the file's own**, because one written
        // down fixes the place under it. A block pasted from a window flown at dawn in a storm has
        // to bring both with it — the light is most of what the frame is — and one from a window at
        // clear noon should leave the view free to be measured under whatever a run names.
        if (spot.mAt.mHour != sDefaultHour)
            block += std::format("hour = {}\n", spot.mAt.mHour);

        if (spot.mAt.mWeather != sDefaultWeather)
            block += std::format("weather = {}\n", spot.mAt.mWeather);

        return block;
    }

    std::vector<BenchSuite> loadSuites(const std::filesystem::path& path)
    {
        Settings::CategorySettingValueMap entries;
        Settings::SettingsFileParser parser;

        // The same parser `views.cfg` is read with, for the same reason: the shape is a section per
        // record and a key per field, and that parser already has tests.
        parser.loadSettingsFile(path, entries);

        std::vector<BenchSuite> suites;
        for (const auto& [key, value] : entries)
        {
            const std::string& section = key.first;
            const std::string& field = key.second;

            if (suites.empty() || suites.back().mName != section)
                suites.push_back(BenchSuite{ .mName = section });

            BenchSuite& suite = suites.back();
            if (field == "views")
                suite.mViews = Rtx::splitNames(value);
            else if (field == "note")
                suite.mNote = value;
            else
                throw std::runtime_error("suite \"" + section + "\" has no field called \"" + field + "\"");
        }

        for (const BenchSuite& suite : suites)
            if (suite.mViews.empty())
                throw std::runtime_error("suite \"" + suite.mName + "\" names no views");

        if (suites.empty())
            throw std::runtime_error(Files::pathToUnicodeString(path) + " defines no suites");

        return suites;
    }

    const BenchSuite* findSuite(const std::vector<BenchSuite>& suites, std::string_view name)
    {
        const auto found
            = std::find_if(suites.begin(), suites.end(), [&](const BenchSuite& s) { return s.mName == name; });

        return found == suites.end() ? nullptr : &*found;
    }

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

        /// One of the ten weathers the content files name, or a throw saying what was written.
        ///
        /// **Checked here rather than at the frame**, for the reason a mistyped view id is: a place
        /// that quietly stood under another sky reports a number against a frame nobody asked for.
        std::string parseWeather(const std::string& view, const std::string& text)
        {
            if (!Rtx::weatherIndex(text).has_value())
                throw std::runtime_error("view \"" + view + "\" has weather \"" + text
                    + "\", which is none of the weathers the content files name");

            return text;
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

                views[at].mRoute = Rtx::Route{
                    .mTo = *end->mOrigin,
                    .mLookTo = *end->mTarget,
                    .mSpeed = paired->second,
                };
            }
        }

        /// One condition, from what the command line named and what the view fixes. `stopFor`
        /// says which of the two wins and why.
        float hourFor(const std::optional<float>& given, const std::optional<float>& fixed)
        {
            return given.has_value() ? *given : fixed.value_or(sDefaultHour);
        }

        std::string weatherFor(const std::optional<std::string>& given, const std::optional<std::string>& fixed)
        {
            return given.has_value() ? *given : fixed.value_or(std::string(sDefaultWeather));
        }
    }

    Rtx::Stop stopFor(
        const View& view, const std::optional<float>& hour, const std::optional<std::string>& weather, const int day)
    {
        Rtx::Stop stop;

        // **The cell where a view names no id**, because a report row and a hash file are keyed on
        // this and neither can be keyed on nothing. `Viewpoint::mView` keeps the raw id, which is
        // what says whether the block a window prints opens a section of its own.
        stop.mName = view.mName.empty() ? view.mCell : view.mName;
        stop.mNote = view.mNote;

        stop.mStand.mCell = view.mCell;
        stop.mStand.mEye = view.mOrigin;
        stop.mStand.mLook = view.mTarget;

        stop.mSky.mHour = hourFor(hour, view.mHour);
        stop.mSky.mWeather = weatherFor(weather, view.mWeather);
        stop.mSky.mDay = day;

        // **A route flies the player, which is what puts a cell arriving into a measurement.**
        // Where it ends is another view's camera, copied into the entry when the file was read.
        stop.mSchedule.mRoute = view.mRoute;

        return stop;
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
            else if (field == "weather")
                view.mWeather = parseWeather(section, value);
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
