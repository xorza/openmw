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

#include "model/benchrun.hpp"
#include "model/cameratrack.hpp"
#include "run.hpp"

namespace RtxTool
{
    /// One key of a film, as a keys file states it: the place a window prints on Home, and what a
    /// film does at it and on the way to it.
    struct FilmKey
    {
        /// Where it stands and under what sky: a stop, whose cell, eye and look `readKeys` requires
        /// and whose hour and weather it settles at the file's own where the block names none, the
        /// rule `describeBlock` leaves them out by. The day stays unsaid where the block names none.
        Stop mStop;

        const std::string& getCell() const { return mStop.mStand.mCell; }
        const osg::Vec3f& getEye() const { return *mStop.mStand.mEye; }
        const osg::Vec3f& getLook() const { return *mStop.mStand.mLook; }
        float getHour() const { return *mStop.mSky.mHour; }
        const std::string& getWeather() const { return *mStop.mSky.mWeather; }

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
    std::vector<FilmKey> readKeys(std::istream& in, std::string source);

    std::vector<FilmKey> loadKeys(const std::filesystem::path& path);

    /// Whether `cell` is an exterior, spelt as a pair of integers the way `--cell` takes one.
    bool isExteriorCell(std::string_view cell);

    /// What paces a film: the command line's, each a default `film --help` states.
    struct FilmPacing
    {
        /// How long one frame of the film stands for: the run's own step (`RunSetup::mStep`), which
        /// every length below is counted in frames by, and `--fps` is one over.
        float mStep = Rtx::sStepSeconds;

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
        float mWarmupSeconds = sWarmupByDefault;

        /// The frame's vertical field of view, in degrees, and its width over its height: what a
        /// pan's pace is read against.
        float mFieldOfView = 60.0f;
        float mAspect = 16.0f / 9.0f;

        /// Which day a take stands on where its first key names none.
        int mDay = 0;

        /// Seconds as a whole count of frames at the step, one at least: `Rtx::BenchSpan`'s count,
        /// which is what the session turns a warm-up's seconds into.
        std::uint32_t framesOf(float seconds) const;

        /// Frames a second, which is what a person reads and what the encoder is told.
        float getRate() const { return 1.0f / mStep; }
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

        /// The keys at their frames, a hold stated as two, for `CameraTrack`.
        std::vector<TrackKey> mTrack;

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
    std::vector<Stop> stopsFor(const FilmPlan& plan, const std::filesystem::path& frames);

    /// What a film's frame `number` is written as in its directory, `000042.png`: six digits, which
    /// is what ffmpeg reads the sequence back by and what `clearFrames` removes.
    std::string frameName(std::uint32_t number);

    /// Removes every file in `frames` that `frameName` could have written, and nothing else: the
    /// encoder reads a sequence up to its first gap, so a longer film's last frames left behind
    /// would play on after this one. Returns how many it removed.
    std::size_t clearFrames(const std::filesystem::path& frames);

    /// The encoder a film is written with: H.264, which every player decodes.
    inline constexpr std::string_view sVideoCodec = "libx264";

    /// Its constant rate factor, the quality it holds: eighteen is what "visually lossless" means
    /// in practice for H.264.
    inline constexpr int sVideoQuality = 18;

    /// The pixel layout every player reads, where H.264's default out of RGB frames is one most
    /// of them do not.
    inline constexpr std::string_view sVideoPixels = "yuv420p";

    /// The ffmpeg line that encodes the frames in `frames` into `video` at `framesPerSecond`, for
    /// the system's shell: `sVideoCodec` at `sVideoQuality` under the slow preset, in
    /// `sVideoPixels`; the index at the front, so a browser plays it before it has it all; and an
    /// odd side padded by one, which H.264 refuses.
    std::string encodeCommand(
        const std::filesystem::path& frames, const std::filesystem::path& video, float framesPerSecond);
}
