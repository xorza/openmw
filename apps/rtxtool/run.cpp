#include "run.hpp"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstddef>
#include <format>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <osg/Math>
#include <osg/Vec3f>

#include <components/files/configurationmanager.hpp>
#include <components/files/conversion.hpp>
#include <components/rtx/skylight.hpp>
#include <components/rtxbench/benchspec.hpp>
#include <components/settings/categories.hpp>
#include <components/settings/parser.hpp>

#include "model/benchrecord.hpp"
#include "model/blockfile.hpp"

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

    std::string shippedDefault(
        const Files::ConfigurationManager& config, const std::string_view category, const std::string_view setting)
    {
        // Parsed once per process: the file does not change under a run, and every framed verb asks
        // for two of its values.
        static Settings::CategorySettingValueMap shipped;
        if (shipped.empty())
        {
            Settings::SettingsFileParser parser;
            parser.loadSettingsFile(config.getActiveConfigPaths().front() / "defaults.bin", shipped, true, false);
        }

        const auto found = shipped.find(Settings::CategorySetting(category, setting));
        if (found == shipped.end())
            throw std::runtime_error(std::format("defaults.bin names no [{}] {}", category, setting));

        return found->second;
    }

    float bearingOf(const Stand& stand)
    {
        const float degrees = osg::RadiansToDegrees(stand.getRotation().z());
        return degrees < 0.0f ? degrees + 360.0f : degrees;
    }

    float climbOf(const Stand& stand)
    {
        return osg::RadiansToDegrees(-stand.getRotation().x());
    }

    std::string describeSpot(const Stop& stop)
    {
        const osg::Vec3f& eye = *stop.mStand.mEye;

        return std::format("# {} at {:.0f}, {:.0f}, {:.0f} — bearing {:.0f}°, climb {:.0f}° — day {}, {}, {}\n",
            stop.mStand.mCell, eye.x(), eye.y(), eye.z(), bearingOf(stop.mStand), climbOf(stop.mStand),
            stop.mSky.mDay.value_or(0), describeHour(stop.mSky.mHour.value_or(sDefaultHour)),
            stop.mSky.mWeather.value_or(std::string(sDefaultWeather)));
    }

    std::string describeBlock(const Stop& stop)
    {
        std::string block = std::format("[{}]\n", slugOf(stop.mName));

        if (!stop.mNote.empty())
            block += std::format("note = {}\n", stop.mNote);

        const osg::Vec3f& eye = *stop.mStand.mEye;
        const osg::Vec3f look = stop.mStand.getLook();
        block += std::format("cell = {}\npos = {}, {}, {}\nlook = {}, {}, {}\n", stop.mStand.mCell, eye.x(), eye.y(),
            eye.z(), look.x(), look.y(), look.z());

        // **Each condition only where the window was not at the file's own**, because one written
        // down fixes the place under it. A block pasted from a window flown at dawn in a storm has
        // to bring both with it — the light is most of what the frame is — and one from a window at
        // clear noon should leave the view free to be measured under whatever a run names.
        if (stop.mSky.mHour.has_value() && *stop.mSky.mHour != sDefaultHour)
            block += std::format("hour = {}\n", *stop.mSky.mHour);

        if (stop.mSky.mWeather.has_value() && *stop.mSky.mWeather != sDefaultWeather)
            block += std::format("weather = {}\n", *stop.mSky.mWeather);

        return block;
    }

    std::string describeCommand(const Stop& stop)
    {
        const osg::Vec3f& eye = *stop.mStand.mEye;
        const osg::Vec3f look = stop.mStand.getLook();

        // Quoted, because an interior's name has spaces and commas in it; `=` on every switch,
        // because a leading minus reads as an option otherwise, as `--pos`'s help says.
        return std::format(
            "# openmw-rtxtool view --cell=\"{}\" --pos={},{},{} --look={},{},{} --hour={} --day={} --weather={}\n",
            stop.mStand.mCell, eye.x(), eye.y(), eye.z(), look.x(), look.y(), look.z(),
            stop.mSky.mHour.value_or(sDefaultHour), stop.mSky.mDay.value_or(0),
            stop.mSky.mWeather.value_or(std::string(sDefaultWeather)));
    }

    std::string describeKey(const Stop& stop)
    {
        return describeBlock(stop) + std::format("day = {}\n", stop.mSky.mDay.value_or(0));
    }

    std::string describeStanding(const Stop& stop)
    {
        return describeSpot(stop) + describeBlock(stop) + describeCommand(stop);
    }

    std::string cellArgument(const bool exterior, const int gridX, const int gridY, const std::string_view name)
    {
        return exterior ? std::format("{},{}", gridX, gridY) : std::string(name);
    }

    void setTurnCrossings(const std::span<const std::string> turnThrough, std::map<std::string, std::string>& fallback)
    {
        const std::string delta = std::format("{}", 1.0f / sTurnSeconds);
        for (const std::string& weather : turnThrough)
        {
            if (!Rtx::weatherIndex(weather).has_value())
                continue;

            fallback.insert_or_assign(std::format("Weather_{}_Transition_Delta", weather), delta);
        }
    }

    std::string_view writeSkyNote(const std::span<char> room, const SkyNote& note)
    {
        // `describeHour`'s minute, so the title and the block a window prints agree on it.
        const int minutes = minuteOfDay(note.mHour);

        // Cut down and never rounded up: a hundred means arrived.
        const int percent = static_cast<int>(note.mCrossed * 100.0f);

        const auto [end, length] = note.mArriving.empty()
            ? std::format_to_n(room.data(), room.size(), "{}, {:02}:{:02}", note.mWeather, minutes / 60, minutes % 60)
            : std::format_to_n(room.data(), room.size(), "{} → {} {}%, {:02}:{:02}", note.mWeather, note.mArriving,
                percent, minutes / 60, minutes % 60);

        assert(static_cast<std::size_t>(length) <= room.size() && "the sky note outgrew its room");
        return std::string_view(room.data(), std::min(static_cast<std::size_t>(length), room.size()));
    }

    std::vector<BenchSuite> loadSuites(const std::filesystem::path& path)
    {
        const BlockFile file = BlockFile::load(path);
        const std::span<const Block> blocks = file.getBlocks();

        std::vector<BenchSuite> suites;
        suites.reserve(blocks.size());
        for (std::size_t at = 0; at < blocks.size(); ++at)
        {
            const Block& block = blocks[at];
            for (std::size_t before = 0; before < at; ++before)
                if (blocks[before].mName == block.mName)
                    file.refuseRepeat(block, blocks[before], "suite");

            BenchSuite& suite = suites.emplace_back(BenchSuite{ .mName = block.mName });
            for (const BlockField& field : block.mFields)
            {
                if (field.mName == "views")
                    suite.mViews = Rtx::splitNames(field.mValue);
                else if (field.mName == "note")
                    suite.mNote = field.mValue;
                else if (field.mName == "settled")
                    suite.mSettled = file.boolean(field);
                else
                    file.refuseUnknown(field, "suite");
            }

            if (suite.mViews.empty())
                file.refuse(block.mLine, std::format("suite \"{}\" names no views", suite.mName));
        }

        if (suites.empty())
            throw std::runtime_error(file.getSource() + " defines no suites");

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
        /// A field a view names another view by, `to` or `like` or the `speed` beside a `to`,
        /// waiting for every view to be read: the view that wrote it, and the field, for its line.
        struct Pending
        {
            std::size_t mView = 0;
            const BlockField* mField = nullptr;
        };

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
        void resolveLikes(const BlockFile& file, std::vector<Stop>& views, std::span<const Pending> likes)
        {
            for (const Pending& like : likes)
            {
                Stop& borrower = views[like.mView];
                const std::string& name = like.mField->mValue;
                if (borrower.mName == name)
                    file.refuse(like.mField->mLine, std::format("view \"{}\" is like itself", name));

                const auto lent = std::find_if(
                    likes.begin(), likes.end(), [&](const Pending& l) { return views[l.mView].mName == name; });
                if (lent != likes.end())
                    file.refuse(like.mField->mLine,
                        std::format("view \"{}\" is like \"{}\", which is itself like another view; only a view "
                                    "that states its own place may be lent",
                            borrower.mName, name));

                const Stop* source = findView(views, name);
                if (source == nullptr)
                    file.refuse(like.mField->mLine,
                        std::format("view \"{}\" is like \"{}\", which is not a view", borrower.mName, name));

                // Written through the vector while `source` points into it, which the check above
                // makes safe: the two are different views and nothing here resizes.
                if (borrower.mStand.mCell.empty())
                    borrower.mStand.mCell = source->mStand.mCell;
                if (!borrower.mStand.mEye.has_value())
                    borrower.mStand.mEye = source->mStand.mEye;
                if (!borrower.mStand.mLook.has_value())
                    borrower.mStand.mLook = source->mStand.mLook;
            }
        }

        /// Pairs each `to` with the view it names and with the `speed` beside it.
        ///
        /// **Both halves are required and neither has a default.** A route with no speed does not
        /// move and a speed with no destination has nowhere to go; either alone is a typo, and
        /// guessing what was meant is how a benchmark measures something other than what was asked
        /// for. The destination must also name its own `pos` and `look`, because a placement derived
        /// from a cell's bounds would need that cell staged to know it.
        void resolveRoutes(const BlockFile& file, std::vector<Stop>& views, std::span<const Pending> ends,
            std::span<const Pending> speeds)
        {
            const auto pairedWith = [](std::span<const Pending> list, std::size_t view) {
                return std::find_if(list.begin(), list.end(), [&](const Pending& p) { return p.mView == view; });
            };

            for (const Pending& speed : speeds)
                if (pairedWith(ends, speed.mView) == ends.end())
                    file.refuse(speed.mField->mLine,
                        std::format("view \"{}\" names a speed but nowhere to go", views[speed.mView].mName));

            for (const Pending& end : ends)
            {
                const std::string& to = end.mField->mValue;
                const auto speed = pairedWith(speeds, end.mView);
                if (speed == speeds.end())
                    file.refuse(end.mField->mLine,
                        std::format("view \"{}\" flies to \"{}\" at no speed", views[end.mView].mName, to));

                const Stop* arrival = findView(views, to);
                if (arrival == nullptr)
                    file.refuse(end.mField->mLine,
                        std::format("view \"{}\" flies to \"{}\", which is not a view", views[end.mView].mName, to));

                if (!arrival->mStand.mEye.has_value() || !arrival->mStand.mLook.has_value())
                    file.refuse(end.mField->mLine,
                        std::format("view \"{}\" flies to \"{}\", which names no pos and look of its own to arrive at",
                            views[end.mView].mName, to));

                views[end.mView].mSchedule.mRoute = Route{
                    .mTo = *arrival->mStand.mEye,
                    .mLookTo = *arrival->mStand.mLook,
                    .mSpeed = file.positive(*speed->mField, "a positive number of units a second"),
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

    Stop stopFor(
        const Stop& view, const std::optional<float>& hour, const std::optional<std::string>& weather, const int day)
    {
        Stop stop = view;

        // **The cell where a view names no id**, because a report row and a hash file are keyed on
        // this and neither can be keyed on nothing.
        if (stop.mName.empty())
            stop.mName = view.mStand.mCell;

        stop.mSky.mHour = hourFor(hour, view.mSky.mHour);
        stop.mSky.mWeather = weatherFor(weather, view.mSky.mWeather);
        stop.mSky.mDay = day;

        return stop;
    }

    std::vector<Stop> loadViews(const std::filesystem::path& path)
    {
        const BlockFile file = BlockFile::load(path);
        const std::span<const Block> blocks = file.getBlocks();

        // **Collected and resolved afterwards, because a route and a likeness can point forwards.**
        // `to` and `like` may name a view that has not been read yet, so the pairing waits until
        // every block is in.
        std::vector<Pending> ends;
        std::vector<Pending> speeds;
        std::vector<Pending> likes;

        std::vector<Stop> views;
        views.reserve(blocks.size());
        for (std::size_t at = 0; at < blocks.size(); ++at)
        {
            const Block& block = blocks[at];
            for (std::size_t before = 0; before < at; ++before)
                if (blocks[before].mName == block.mName)
                    file.refuseRepeat(block, blocks[before], "view");

            Stop& view = views.emplace_back(Stop{ .mName = block.mName });
            for (const BlockField& field : block.mFields)
            {
                if (file.readPlace(field, view))
                    continue;

                if (field.mName == "to")
                    ends.push_back(Pending{ .mView = at, .mField = &field });
                else if (field.mName == "speed")
                    speeds.push_back(Pending{ .mView = at, .mField = &field });
                else if (field.mName == "like")
                    likes.push_back(Pending{ .mView = at, .mField = &field });
                else
                    file.refuseUnknown(field, "view");
            }
        }

        if (views.empty())
            throw std::runtime_error(file.getSource() + " defines no views");

        // Before the cell is demanded and before a route is paired: a borrower takes both from what
        // it is like, and either check run first would reject a view that is about to be complete.
        resolveLikes(file, views, likes);

        for (std::size_t at = 0; at < views.size(); ++at)
            if (views[at].mStand.mCell.empty())
                file.refuse(blocks[at].mLine, std::format("view \"{}\" names no cell", views[at].mName));

        resolveRoutes(file, views, ends, speeds);
        return views;
    }

    const Stop* findView(const std::vector<Stop>& views, std::string_view name)
    {
        const auto found = std::find_if(views.begin(), views.end(), [&](const Stop& v) { return v.mName == name; });
        return found == views.end() ? nullptr : &*found;
    }

    std::vector<Stop> chooseViews(const std::vector<Stop>& views, const std::vector<std::string>& named)
    {
        // **"all" is a name nothing may take, and it means every view.** `bench` reaches this
        // through a suite as well, so the word has to mean the same on either road in.
        if (named.empty() || (named.size() == 1 && named.front() == "all"))
            return views;

        std::vector<Stop> chosen;
        chosen.reserve(named.size());
        for (const std::string& name : named)
        {
            const Stop* view = findView(views, name);
            if (view == nullptr)
                throw std::runtime_error("no view is called \"" + name + "\"; --list-views prints them");

            chosen.push_back(*view);
        }

        return chosen;
    }
}
