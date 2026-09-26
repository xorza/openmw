#include "film.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <osg/Math>

#include <components/files/conversion.hpp>
#include <components/rtx/contract.hpp>
#include <components/rtx/skylight.hpp>
#include <components/rtxbench/benchrecord.hpp>
#include <components/rtxbench/benchspec.hpp>

#include "options.hpp"

namespace RtxTool
{
    namespace
    {
        std::string_view trimmed(std::string_view text)
        {
            const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
            while (!text.empty() && blank(text.front()))
                text.remove_prefix(1);
            while (!text.empty() && blank(text.back()))
                text.remove_suffix(1);
            return text;
        }

        /// Reads one keys file, a line at a time, naming the line in whatever it refuses.
        ///
        /// **Its own reader and not the settings parser `views.cfg` is read with**, because that
        /// one keeps a map: two sections of one name are one section there, and a window pressed
        /// twice at one place writes two keys under one name.
        class KeyReader
        {
        public:
            explicit KeyReader(std::string_view source)
                : mSource(source)
            {
            }

            [[noreturn]] void refuse(const std::string& why) const { refuseAt(mLine, why); }

            [[noreturn]] void refuseAt(std::size_t line, const std::string& why) const
            {
                throw std::runtime_error(std::format("{}:{}: {}", mSource, line, why));
            }

            float number(std::string_view field, std::string_view text) const
            {
                const std::optional<float> value = parseFloat(text);
                if (!value.has_value())
                    refuse(std::format("{} \"{}\" is not a number", field, text));
                return *value;
            }

            /// Starts a key at the line being read.
            FilmKey open(std::string_view name)
            {
                mEye = false;
                mLook = false;
                return FilmKey{ .mName = std::string(name), .mLine = mLine };
            }

            void read(std::string_view field, std::string_view value, FilmKey& key)
            {
                mEye = mEye || field == "pos";
                mLook = mLook || field == "look";

                if (field == "cell")
                    key.mCell = value;
                else if (field == "pos")
                    key.mEye = vector(field, value);
                else if (field == "look")
                    key.mLook = vector(field, value);
                else if (field == "note")
                    key.mNote = value;
                else if (field == "hour")
                {
                    key.mHour = number(field, value);
                    if (!(key.mHour >= 0.0f) || !(key.mHour < 24.0f))
                        refuse(std::format("hour \"{}\" is not from 0 up to but not including 24", value));
                }
                else if (field == "weather")
                {
                    if (!Rtx::weatherIndex(value).has_value())
                        refuse(std::format("weather \"{}\" is none of the weathers the content files name", value));
                    key.mWeather = value;
                }
                else if (field == "day")
                {
                    int day = 0;
                    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), day);
                    if (error != std::errc() || end != value.data() + value.size() || day < 0)
                        refuse(std::format("day \"{}\" is not a whole number of days from nought", value));
                    key.mDay = day;
                }
                else if (field == "seconds")
                {
                    key.mSeconds = number(field, value);
                    if (!(*key.mSeconds > 0.0f))
                        refuse(std::format("seconds \"{}\" is not a length of time", value));
                }
                else if (field == "hold")
                {
                    key.mHold = number(field, value);
                    if (!(key.mHold >= 0.0f))
                        refuse(std::format("hold \"{}\" is not a length of time", value));
                }
                else if (field == "cut")
                {
                    if (value != "true" && value != "false")
                        refuse(std::format("cut \"{}\" is not true or false", value));
                    key.mCut = value == "true";
                }
                else
                    refuse(std::format("a key has no field called \"{}\"", field));
            }

            /// Refuses `key` unless it states a place a camera can stand at. `pos` and `look` are
            /// the pair Home always writes, and a key without them has no facing to fly through.
            void close(const FilmKey& key) const
            {
                if (key.mCell.empty())
                    refuseAt(key.mLine, std::format("key \"{}\" names no cell", key.mName));
                if (!mEye || !mLook)
                    refuseAt(key.mLine, std::format("key \"{}\" names no pos and look", key.mName));
            }

            std::size_t mLine = 0;

        private:
            osg::Vec3f vector(std::string_view field, std::string_view value) const
            {
                try
                {
                    return *parseVec3(value, field);
                }
                catch (const std::runtime_error& error)
                {
                    refuse(error.what());
                }
            }

            std::string_view mSource;

            /// Whether the key being read has stated each of the two.
            bool mEye = false;
            bool mLook = false;
        };
    }

    std::vector<FilmKey> readKeys(std::istream& in, const std::string_view source)
    {
        KeyReader reader(source);
        std::vector<FilmKey> keys;

        for (std::string line; std::getline(in, line);)
        {
            ++reader.mLine;
            const std::string_view text = trimmed(line);
            if (text.empty() || text.front() == '#')
                continue;

            if (text.front() == '[')
            {
                if (text.back() != ']')
                    reader.refuse("a section's name is not closed by ]");

                if (!keys.empty())
                    reader.close(keys.back());
                keys.push_back(reader.open(trimmed(text.substr(1, text.size() - 2))));
                continue;
            }

            const std::size_t equals = text.find('=');
            if (equals == std::string_view::npos)
                reader.refuse(std::format("\"{}\" is neither a [key], a field = value, nor a # comment", text));
            if (keys.empty())
                reader.refuse("a field comes before the first [key]");

            reader.read(trimmed(text.substr(0, equals)), trimmed(text.substr(equals + 1)), keys.back());
        }

        if (keys.empty())
            throw std::runtime_error(std::format("{} states no keys", source));
        reader.close(keys.back());

        return keys;
    }

    std::vector<FilmKey> loadKeys(const std::filesystem::path& path)
    {
        std::ifstream in(path);
        if (!in)
            throw std::runtime_error("cannot read the keys in " + Files::pathToUnicodeString(path));

        return readKeys(in, Files::pathToUnicodeString(path));
    }

    bool isExteriorCell(const std::string_view cell)
    {
        const std::size_t comma = cell.find(',');
        if (comma == std::string_view::npos)
            return false;

        const auto whole = [](std::string_view text) {
            text = trimmed(text);
            int value = 0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            return !text.empty() && error == std::errc() && end == text.data() + text.size();
        };

        return whole(cell.substr(0, comma)) && whole(cell.substr(comma + 1));
    }

    std::uint32_t FilmPacing::framesOf(const float seconds) const
    {
        return std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(seconds * mFramesPerSecond)));
    }

    std::uint32_t FilmPlan::getFrames() const
    {
        return mTakes.empty() ? 0 : mTakes.back().mFirstFrame + mTakes.back().getFrames();
    }

    namespace
    {
        osg::Vec3f rotationOf(const FilmKey& key)
        {
            return Rtx::Stand{ .mEye = key.mEye, .mLook = key.mLook }.getRotation();
        }

        /// Why `key` cuts away from `before`, or nothing where the camera flies from one to the
        /// other. The jump is written where the distance decided.
        std::optional<FilmCut> cutBetween(const FilmKey& before, const FilmKey& key, float cutDistance, float& jump)
        {
            if (key.mCut.has_value())
                return *key.mCut ? std::optional(FilmCut::Asked) : std::nullopt;

            const bool outside = isExteriorCell(key.mCell);
            if (isExteriorCell(before.mCell) != outside)
                return outside ? FilmCut::Outdoors : FilmCut::Indoors;

            if (!outside && key.mCell != before.mCell)
                return FilmCut::Interior;

            jump = (key.mEye - before.mEye).length();
            if (jump > cutDistance)
                return FilmCut::Distance;

            return std::nullopt;
        }

        /// The segment from `from` into `to`: every change it makes, and the longest of the lengths
        /// they ask for, or the one the key gives.
        FilmSegment timeSegment(const FilmKey& from, const FilmKey& to, std::size_t index, const FilmPacing& pacing)
        {
            FilmSegment segment{ .mTo = index };

            const osg::Vec3f turnFrom = rotationOf(from);
            const osg::Vec3f turnTo = rotationOf(to);
            const float yaw = std::abs(Rtx::shortestTurn(turnFrom.z(), turnTo.z()));
            const float pitch = std::abs(turnTo.x() - turnFrom.x());

            segment.mDistance = (to.mEye - from.mEye).length();
            segment.mTurnDegrees = osg::RadiansToDegrees(std::max(yaw, pitch));
            segment.mHours = Rtx::hoursForward(from.mHour, to.mHour);

            // A pan is paced against the picture it sweeps: one image width in `mPanSeconds`
            // across, one image height up or down.
            const float vertical = osg::DegreesToRadians(pacing.mFieldOfView);
            const float horizontal = 2.0f * std::atan(std::tan(vertical / 2.0f) * pacing.mAspect);

            const std::pair<float, FilmPace> asks[] = {
                { segment.mDistance / pacing.mSpeed, FilmPace::Distance },
                { std::max(yaw / horizontal, pitch / vertical) * pacing.mPanSeconds, FilmPace::Turn },
                { segment.mHours * pacing.mHourSeconds, FilmPace::Clock },
                { from.mWeather != to.mWeather ? pacing.mCrossingSeconds : 0.0f, FilmPace::Weather },
            };

            float seconds = 0.0f;
            for (const auto& [asked, pace] : asks)
                if (asked > seconds)
                {
                    seconds = asked;
                    segment.mPace = pace;
                }

            if (seconds <= 0.0f)
                seconds = pacing.mStillSeconds;

            if (to.mSeconds.has_value())
            {
                seconds = *to.mSeconds;
                segment.mPace = FilmPace::Given;
            }

            segment.mFrames = pacing.framesOf(seconds);
            return segment;
        }

        Rtx::TrackKey trackKey(const FilmKey& key, std::uint32_t frame, bool rests)
        {
            return Rtx::TrackKey{ .mFrame = frame,
                .mEye = key.mEye,
                .mRotation = rotationOf(key),
                .mHour = key.mHour,
                .mWeather = *Rtx::weatherIndex(key.mWeather),
                .mRests = rests };
        }

        /// The keys of `take` at their frames. A hold is the key twice, resting on both, so the
        /// camera stops on it and sets off again; a take of one key holds it for a still at least.
        void layTrack(const FilmPlan& plan, FilmTake& take)
        {
            const bool alone = take.mEnd - take.mFirst == 1;
            std::uint32_t frame = 0;
            for (std::size_t at = take.mFirst; at < take.mEnd; ++at)
            {
                const FilmKey& key = plan.mKeys[at];
                if (at > take.mFirst)
                    frame += take.mSegments[at - take.mFirst - 1].mFrames;

                const float hold = alone ? std::max(key.mHold, plan.mPacing.mStillSeconds) : key.mHold;
                take.mTrack.push_back(trackKey(key, frame, hold > 0.0f));
                if (hold > 0.0f)
                {
                    frame += plan.mPacing.framesOf(hold);
                    take.mTrack.push_back(trackKey(key, frame, true));
                }
            }
        }

        std::string_view describeCut(const FilmCut cut)
        {
            switch (cut)
            {
                case FilmCut::First:
                    return "the first key";
                case FilmCut::Asked:
                    return "the key asks for a cut";
                case FilmCut::Indoors:
                    return "outside to inside";
                case FilmCut::Outdoors:
                    return "inside to outside";
                case FilmCut::Interior:
                    return "another interior";
                case FilmCut::Distance:
                    return "too far to fly";
            }
            Rtx::broken("a cut with no name");
        }

        /// What set a segment's length, in the words and numbers that set it.
        std::string describePace(const FilmSegment& segment, const FilmKey& to, const FilmPacing& pacing)
        {
            switch (segment.mPace)
            {
                case FilmPace::Given:
                    return "as the key says";
                case FilmPace::Distance:
                    return std::format("{:.0f} units at {:.0f} a second", segment.mDistance, pacing.mSpeed);
                case FilmPace::Turn:
                    return std::format("a turn of {:.0f}°", segment.mTurnDegrees);
                case FilmPace::Clock:
                    return std::format("{:.2f} hours of clock", segment.mHours);
                case FilmPace::Weather:
                    return std::format("the sky crossing into {}", to.mWeather);
                case FilmPace::Still:
                    return "nothing changes";
            }
            Rtx::broken("a pace with no name");
        }
    }

    FilmPlan planFilm(std::vector<FilmKey> keys, const FilmPacing& pacing)
    {
        if (keys.empty())
            throw std::runtime_error("a film needs a key");

        FilmPlan plan{ .mKeys = std::move(keys), .mPacing = pacing };
        for (std::size_t at = 0; at < plan.mKeys.size(); ++at)
        {
            float jump = 0.0f;
            const std::optional<FilmCut> cut
                = at == 0 ? FilmCut::First : cutBetween(plan.mKeys[at - 1], plan.mKeys[at], pacing.mCutDistance, jump);

            if (cut.has_value())
            {
                plan.mTakes.push_back(FilmTake{ .mFirst = at, .mEnd = at, .mCut = *cut, .mJump = jump });
            }
            else
            {
                FilmTake& take = plan.mTakes.back();
                take.mSegments.push_back(timeSegment(plan.mKeys[at - 1], plan.mKeys[at], at, pacing));
            }

            plan.mTakes.back().mEnd = at + 1;
        }

        std::uint32_t first = 0;
        for (FilmTake& take : plan.mTakes)
        {
            layTrack(plan, take);
            take.mFirstFrame = first;
            first += take.getFrames();
        }

        return plan;
    }

    std::string describePlan(const FilmPlan& plan)
    {
        const float rate = plan.mPacing.mFramesPerSecond;
        const auto seconds = [&](std::uint32_t frames) { return static_cast<float>(frames) / rate; };

        std::string text = std::format("film: {} keys, {} takes, {} frames, {:.1f} s at {} frames a second\n",
            plan.mKeys.size(), plan.mTakes.size(), plan.getFrames(), seconds(plan.getFrames()), rate);

        for (std::size_t number = 0; number < plan.mTakes.size(); ++number)
        {
            const FilmTake& take = plan.mTakes[number];
            const FilmKey& first = plan.mKeys[take.mFirst];

            text += std::format("\ntake {}, from frame {}: {:.1f} s, cut in: {}", number + 1, take.mFirstFrame,
                seconds(take.getFrames()), describeCut(take.mCut));
            if (take.mCut == FilmCut::Distance)
                text += std::format(", {:.0f} units", take.mJump);
            text += '\n';

            text += std::format("  {:<28} {} {}, {}{}\n", first.mName, first.mCell, Rtx::describeHour(first.mHour),
                first.mWeather, first.mHold > 0.0f ? std::format(", holds {:.1f} s", first.mHold) : std::string());

            for (const FilmSegment& segment : take.mSegments)
            {
                const FilmKey& key = plan.mKeys[segment.mTo];

                text += std::format("  -> {:<25} {:6.1f} s  {} {}, {}{}  ({})\n", key.mName, seconds(segment.mFrames),
                    Rtx::describeHour(key.mHour), key.mWeather,
                    key.mHold > 0.0f ? std::format("holds {:.1f} s, ", key.mHold) : std::string(), key.mCell,
                    describePace(segment, key, plan.mPacing));
            }
        }

        return text;
    }

    std::vector<Rtx::Stop> stopsFor(const FilmPlan& plan, const std::filesystem::path& frames)
    {
        const std::uint32_t warmup
            = static_cast<std::uint32_t>(std::lround(plan.mPacing.mWarmupSeconds * plan.mPacing.mFramesPerSecond));

        std::vector<Rtx::Stop> stops;
        stops.reserve(plan.mTakes.size());
        for (std::size_t number = 0; number < plan.mTakes.size(); ++number)
        {
            const FilmTake& take = plan.mTakes[number];
            const FilmKey& first = plan.mKeys[take.mFirst];

            Rtx::Stop& stop = stops.emplace_back();
            stop.mName = std::format("take-{}-{}", number + 1, first.mName);
            stop.mNote = first.mNote;
            stop.mStand = Rtx::Stand{ .mCell = first.mCell, .mEye = first.mEye, .mLook = first.mLook };
            stop.mSky.mHour = first.mHour;
            stop.mSky.mDay = first.mDay.value_or(plan.mPacing.mDay);
            stop.mSky.mWeather = first.mWeather;
            stop.mSchedule.mSpec.mWarm = Rtx::BenchSpan{ .mFrames = warmup };
            stop.mSchedule.mSpec.mRun = Rtx::BenchSpan{ .mFrames = take.getFrames() };
            stop.mSchedule.mTrack.emplace(take.mTrack);
            stop.mActions.mFilm = Rtx::Actions::Film{ .mDirectory = frames, .mFirst = take.mFirstFrame };
        }

        return stops;
    }

    namespace
    {
        constexpr std::size_t sFrameDigits = 6;
        constexpr std::string_view sFrameExtension = ".png";
    }

    std::string frameName(const std::uint32_t number)
    {
        return std::format("{:0{}}{}", number, sFrameDigits, sFrameExtension);
    }

    std::size_t clearFrames(const std::filesystem::path& frames)
    {
        if (!std::filesystem::is_directory(frames))
            return 0;

        const auto written = [](const std::string& name) {
            return name.size() == sFrameDigits + sFrameExtension.size() && name.ends_with(sFrameExtension)
                && std::all_of(name.begin(), name.begin() + sFrameDigits, [](char c) { return c >= '0' && c <= '9'; });
        };

        std::vector<std::filesystem::path> doomed;
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(frames))
            if (entry.is_regular_file() && written(entry.path().filename().string()))
                doomed.push_back(entry.path());

        for (const std::filesystem::path& path : doomed)
            std::filesystem::remove(path);

        return doomed.size();
    }

    namespace
    {
        /// `path` as one word of the system's shell: in single quotes for a POSIX one, where
        /// nothing inside them is expanded, and in double quotes for `cmd`, which has no others.
        std::string shellWord(const std::filesystem::path& path)
        {
            const std::string text = Files::pathToUnicodeString(path);
#if defined(_WIN32)
            return '"' + text + '"';
#else
            std::string word = "'";
            for (const char c : text)
                word += c == '\'' ? std::string("'\\''") : std::string(1, c);
            return word + "'";
#endif
        }
    }

    std::string encodeCommand(
        const std::filesystem::path& frames, const std::filesystem::path& video, const float framesPerSecond)
    {
        const std::string line = std::format(
            "ffmpeg -hide_banner -loglevel warning -y -framerate {} -i {} -vf \"pad=ceil(iw/2)*2:ceil(ih/2)*2\" "
            "-c:v libx264 -preset slow -crf 18 -pix_fmt yuv420p -movflags +faststart {}",
            framesPerSecond, shellWord(frames / std::format("%0{}d{}", sFrameDigits, sFrameExtension)),
            shellWord(video));

#if defined(_WIN32)
        // `cmd /c` takes the whole line in one more pair of quotes.
        return '"' + line + '"';
#else
        return line;
#endif
    }
}
