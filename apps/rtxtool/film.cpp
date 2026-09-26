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

#include "model/benchrecord.hpp"
#include "model/benchspec.hpp"
#include "model/blockfile.hpp"

namespace RtxTool
{
    std::vector<FilmKey> readKeys(std::istream& in, std::string source)
    {
        const BlockFile file(in, std::move(source));

        std::vector<FilmKey> keys;
        keys.reserve(file.getBlocks().size());
        for (const Block& block : file.getBlocks())
        {
            FilmKey& key = keys.emplace_back(FilmKey{ .mStop = Stop{ .mName = block.mName }, .mLine = block.mLine });
            for (const BlockField& field : block.mFields)
            {
                if (file.readPlace(field, key.mStop))
                    continue;

                if (field.mName == "day")
                    key.mStop.mSky.mDay = file.day(field);
                else if (field.mName == "seconds")
                    key.mSeconds = file.positive(field, "a length of time");
                else if (field.mName == "hold")
                    key.mHold = file.notNegative(field, "a length of time");
                else if (field.mName == "cut")
                    key.mCut = file.boolean(field);
                else
                    file.refuseUnknown(field, "key");
            }

            // `pos` and `look` are the pair Home always writes, and a key without them has no
            // facing to fly through.
            const Stand& stand = key.mStop.mStand;
            if (stand.mCell.empty())
                file.refuse(block.mLine, std::format("key \"{}\" names no cell", block.mName));
            if (!stand.mEye.has_value() || !stand.mLook.has_value())
                file.refuse(block.mLine, std::format("key \"{}\" names no pos and look", block.mName));

            StopSky& sky = key.mStop.mSky;
            sky.mHour = sky.mHour.value_or(sDefaultHour);
            sky.mWeather = sky.mWeather.value_or(std::string(sDefaultWeather));
        }

        if (keys.empty())
            throw std::runtime_error(std::format("{} states no keys", file.getSource()));

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
        return std::max(1u, BenchSpan{ .mSeconds = seconds }.getFrames(mStep));
    }

    std::uint32_t FilmPlan::getFrames() const
    {
        return mTakes.empty() ? 0 : mTakes.back().mFirstFrame + mTakes.back().getFrames();
    }

    namespace
    {
        osg::Vec3f rotationOf(const FilmKey& key)
        {
            return key.mStop.mStand.getRotation();
        }

        /// Why `key` cuts away from `before`, or nothing where the camera flies from one to the
        /// other. The jump is written where the distance decided.
        std::optional<FilmCut> cutBetween(const FilmKey& before, const FilmKey& key, float cutDistance, float& jump)
        {
            if (key.mCut.has_value())
                return *key.mCut ? std::optional(FilmCut::Asked) : std::nullopt;

            const bool outside = isExteriorCell(key.getCell());
            if (isExteriorCell(before.getCell()) != outside)
                return outside ? FilmCut::Outdoors : FilmCut::Indoors;

            if (!outside && key.getCell() != before.getCell())
                return FilmCut::Interior;

            jump = (key.getEye() - before.getEye()).length();
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
            const float yaw = std::abs(shortestTurn(turnFrom.z(), turnTo.z()));
            const float pitch = std::abs(turnTo.x() - turnFrom.x());

            segment.mDistance = (to.getEye() - from.getEye()).length();
            segment.mTurnDegrees = osg::RadiansToDegrees(std::max(yaw, pitch));
            segment.mHours = hoursForward(from.getHour(), to.getHour());

            // A pan is paced against the picture it sweeps: one image width in `mPanSeconds`
            // across, one image height up or down.
            const float vertical = osg::DegreesToRadians(pacing.mFieldOfView);
            const float horizontal = 2.0f * std::atan(std::tan(vertical / 2.0f) * pacing.mAspect);

            const std::pair<float, FilmPace> asks[] = {
                { segment.mDistance / pacing.mSpeed, FilmPace::Distance },
                { std::max(yaw / horizontal, pitch / vertical) * pacing.mPanSeconds, FilmPace::Turn },
                { segment.mHours * pacing.mHourSeconds, FilmPace::Clock },
                { from.getWeather() != to.getWeather() ? pacing.mCrossingSeconds : 0.0f, FilmPace::Weather },
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

        TrackKey trackKey(const FilmKey& key, std::uint32_t frame, bool rests)
        {
            return TrackKey{ .mFrame = frame,
                .mEye = key.getEye(),
                .mRotation = rotationOf(key),
                .mHour = key.getHour(),
                .mWeather = *Rtx::weatherIndex(key.getWeather()),
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
                    return std::format("the sky crossing into {}", to.getWeather());
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
        const auto seconds = [&](std::uint32_t frames) { return static_cast<float>(frames) * plan.mPacing.mStep; };

        // `:g`, because the rate is one over the step and reads back as 59.999996 otherwise.
        std::string text = std::format("film: {} keys, {} takes, {} frames, {:.1f} s at {:g} frames a second\n",
            plan.mKeys.size(), plan.mTakes.size(), plan.getFrames(), seconds(plan.getFrames()), plan.mPacing.getRate());

        for (std::size_t number = 0; number < plan.mTakes.size(); ++number)
        {
            const FilmTake& take = plan.mTakes[number];
            const FilmKey& first = plan.mKeys[take.mFirst];

            text += std::format("\ntake {}, from frame {}: {:.1f} s, cut in: {}", number + 1, take.mFirstFrame,
                seconds(take.getFrames()), describeCut(take.mCut));
            if (take.mCut == FilmCut::Distance)
                text += std::format(", {:.0f} units", take.mJump);
            text += '\n';

            text += std::format("  {:<28} {} {}, {}{}\n", first.mStop.mName, first.getCell(),
                describeHour(first.getHour()), first.getWeather(),
                first.mHold > 0.0f ? std::format(", holds {:.1f} s", first.mHold) : std::string());

            for (const FilmSegment& segment : take.mSegments)
            {
                const FilmKey& key = plan.mKeys[segment.mTo];

                text += std::format("  -> {:<25} {:6.1f} s  {} {}, {}{}  ({})\n", key.mStop.mName,
                    seconds(segment.mFrames), describeHour(key.getHour()), key.getWeather(),
                    key.mHold > 0.0f ? std::format("holds {:.1f} s, ", key.mHold) : std::string(), key.getCell(),
                    describePace(segment, key, plan.mPacing));
            }
        }

        return text;
    }

    std::vector<Stop> stopsFor(const FilmPlan& plan, const std::filesystem::path& frames)
    {

        std::vector<Stop> stops;
        stops.reserve(plan.mTakes.size());
        for (std::size_t number = 0; number < plan.mTakes.size(); ++number)
        {
            const FilmTake& take = plan.mTakes[number];
            const FilmKey& first = plan.mKeys[take.mFirst];

            Stop& stop = stops.emplace_back();
            stop = first.mStop;
            stop.mName = std::format("take-{}-{}", number + 1, first.mStop.mName);
            stop.mSky.mDay = first.mStop.mSky.mDay.value_or(plan.mPacing.mDay);
            stop.mSchedule.mSpec.mWarm = BenchSpan{ .mSeconds = plan.mPacing.mWarmupSeconds };
            stop.mSchedule.mSpec.mRun = BenchSpan{ .mFrames = take.getFrames() };
            stop.mSchedule.mTrack.emplace(take.mTrack);
            stop.mActions.mFilm = Actions::Film{ .mDirectory = frames, .mFirst = take.mFirstFrame };
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
            "ffmpeg -hide_banner -loglevel warning -y -framerate {:g} -i {} -vf \"pad=ceil(iw/2)*2:ceil(ih/2)*2\" "
            "-c:v {} -preset slow -crf {} -pix_fmt {} -movflags +faststart {}",
            framesPerSecond, shellWord(frames / std::format("%0{}d{}", sFrameDigits, sFrameExtension)), sVideoCodec,
            sVideoQuality, sVideoPixels, shellWord(video));

#if defined(_WIN32)
        // `cmd /c` takes the whole line in one more pair of quotes.
        return '"' + line + '"';
#else
        return line;
#endif
    }
}
