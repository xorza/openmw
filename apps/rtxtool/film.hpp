#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <istream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <osg/Vec3f>

#include <components/rtxbench/benchrun.hpp>
#include <components/rtxbench/cameratrack.hpp>

#include "run.hpp"

namespace RtxTool
{
    /// One key of a film, as a keys file states it: the block a window prints on Home, and what a
    /// film does at it and on the way to it.
    struct FilmKey
    {
        std::string mName;
        std::string mNote;

        std::string mCell;
        osg::Vec3f mEye;
        osg::Vec3f mLook;

        /// The file's own hour and weather where the block names none, which is the rule
        /// `describeBlock` leaves them out by.
        float mHour = sDefaultHour;
        std::string mWeather = std::string(sDefaultWeather);
        std::optional<int> mDay;

        /// How long the flight to this key takes, in place of the length its changes derive.
        std::optional<float> mSeconds;

        /// How long the camera rests here.
        float mHold = 0.0f;

        /// Whether a cut comes before this key: forced, forbidden, or left to the distance.
        std::optional<bool> mCut;

        /// The line the key's section opens on, which a message about it names.
        std::size_t mLine = 0;
    };

    /// The keys `in` states, in order. A section name may repeat, since a file is a list of keys
    /// and a window pressed twice in one place names both after it. Throws naming `source` and the
    /// line for anything malformed: a key misread is a film of somewhere else.
    std::vector<FilmKey> readKeys(std::istream& in, std::string_view source);

    std::vector<FilmKey> loadKeys(const std::filesystem::path& path);

    /// Whether `cell` is an exterior, spelt as a pair of integers the way `--cell` takes one.
    bool isExteriorCell(std::string_view cell);

    /// What paces a film: the command line's, each a default `film --help` states.
    struct FilmPacing
    {
        float mFramesPerSecond = 60.0f;

        /// World units a second the eye flies at between two keys.
        float mSpeed = 800.0f;

        /// Seconds a pan takes to turn one image width, the established limit for judder.
        float mPanSeconds = 7.0f;

        /// Seconds of film a game hour takes, where two keys' hours differ.
        float mHourSeconds = 2.0f;

        /// The least a crossing into another weather takes.
        float mCrossingSeconds = 8.0f;

        /// How long a key that nothing leads to or from stands, and a segment where nothing changes.
        float mStillSeconds = 4.0f;

        /// How far apart two keys of one space can be and still be flown between.
        float mCutDistance = 16384.0f;

        /// What is drawn and not written after a cut.
        float mWarmupSeconds = 2.0f;

        /// The frame's vertical field of view, in degrees, and its width over its height: what a
        /// pan's pace is read against.
        float mFieldOfView = 60.0f;
        float mAspect = 16.0f / 9.0f;

        /// Which day a take stands on where its first key names none.
        int mDay = 0;

        /// Seconds as a whole count of frames, one at least.
        std::uint32_t framesOf(float seconds) const;
    };

    /// Which of a segment's changes set its length.
    enum class FilmPace
    {
        Given,
        Distance,
        Turn,
        Clock,
        Weather,
        Still,
    };

    /// The flight from one key to the next, as the plan timed it.
    struct FilmSegment
    {
        /// The key it arrives at.
        std::size_t mTo = 0;

        std::uint32_t mFrames = 0;
        FilmPace mPace = FilmPace::Still;

        /// What changes over it: how far the eye moves, how far it turns, and how many hours the
        /// clock runs.
        float mDistance = 0.0f;
        float mTurnDegrees = 0.0f;
        float mHours = 0.0f;
    };

    /// Why a take begins with a cut.
    enum class FilmCut
    {
        First,
        Asked,
        Indoors,
        Outdoors,
        Interior,
        Distance,
    };

    /// A run of keys with no cut between them: one flight.
    struct FilmTake
    {
        /// Its keys, as indices into the plan's, from `mFirst` up to but not including `mEnd`.
        std::size_t mFirst = 0;
        std::size_t mEnd = 0;

        FilmCut mCut = FilmCut::First;

        /// How far the cut jumped, where the distance was the reason.
        float mJump = 0.0f;

        /// The segment into each key after the first.
        std::vector<FilmSegment> mSegments;

        /// The keys at their frames, a hold stated as two, for `Rtx::CameraTrack`.
        std::vector<Rtx::TrackKey> mTrack;

        /// The number of the take's first frame in the film.
        std::uint32_t mFirstFrame = 0;

        std::uint32_t getFrames() const { return mTrack.back().mFrame + 1; }
    };

    /// A keys file split into takes and timed, before a frame is drawn.
    struct FilmPlan
    {
        std::vector<FilmKey> mKeys;
        std::vector<FilmTake> mTakes;
        FilmPacing mPacing;

        std::uint32_t getFrames() const;
    };

    /// Splits `keys` into takes and times each segment by the longest of what changes over it,
    /// rounded to whole frames. Throws where there is no key.
    FilmPlan planFilm(std::vector<FilmKey> keys, const FilmPacing& pacing);

    /// The plan as a person reads it before committing an hour of rendering to it: every take, why
    /// it cuts, every segment, how long it takes and what set that.
    std::string describePlan(const FilmPlan& plan);

    /// One stop per take, writing its frames into `frames` numbered through the whole film.
    std::vector<Rtx::Stop> stopsFor(const FilmPlan& plan, const std::filesystem::path& frames);

    /// What a film's frame `number` is written as in its directory, `000042.png`: six digits, which
    /// is what ffmpeg reads the sequence back by and what `clearFrames` removes.
    std::string frameName(std::uint32_t number);

    /// Removes every file in `frames` that `frameName` could have written, and nothing else: the
    /// encoder reads a sequence up to its first gap, so a longer film's last frames left behind
    /// would play on after this one. Returns how many it removed.
    std::size_t clearFrames(const std::filesystem::path& frames);

    /// The ffmpeg line that encodes the frames in `frames` into `video` at `framesPerSecond`, for
    /// the system's shell. H.264 at CRF 18, which is what "visually lossless" means in practice,
    /// under the slow preset; yuv420p, which every player decodes; the index at the front, so a
    /// browser plays it before it has it all; and an odd side padded by one, which H.264 refuses.
    std::string encodeCommand(
        const std::filesystem::path& frames, const std::filesystem::path& video, float framesPerSecond);
}
