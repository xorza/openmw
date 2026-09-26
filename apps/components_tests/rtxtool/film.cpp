#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Math>
#include <osg/Vec3f>

#include <apps/rtxtool/film.hpp>
#include <apps/rtxtool/run.hpp>
#include <components/rtx/skylight.hpp>
#include <components/rtxbench/benchrun.hpp>
#include <components/testing/util.hpp>

namespace RtxTool
{
    namespace
    {
        std::vector<FilmKey> read(const std::string& text)
        {
            std::istringstream in(text);
            return readKeys(in, "tour.keys");
        }

        std::string refusal(const std::string& text)
        {
            try
            {
                read(text);
            }
            catch (const std::runtime_error& error)
            {
                return error.what();
            }
            return "nothing was refused";
        }

        FilmKey keyAt(std::string name, std::string cell, osg::Vec3f eye, osg::Vec3f look = osg::Vec3f(0, 1e4f, 0))
        {
            return FilmKey{ .mName = std::move(name), .mCell = std::move(cell), .mEye = eye, .mLook = eye + look };
        }

        /// Ten frames a second, a hundred units a second, and a frame whose horizontal field is
        /// exactly ninety degrees: `2·atan(tan(30°)·√3)` is `2·atan(1)`. A quarter turn is then one
        /// image width, seven seconds, and a pitch of thirty degrees half an image height, 3.5.
        FilmPacing pacingForTests()
        {
            FilmPacing pacing;
            pacing.mFramesPerSecond = 10.0f;
            pacing.mSpeed = 100.0f;
            pacing.mAspect = std::sqrt(3.0f);
            return pacing;
        }

        /// What Home prints, pasted whole, reads back as the place it printed: the comment lines go,
        /// the hour and the weather the block leaves out are the file's own, and two keys may share
        /// a name. What `view --keys` appends reads back with its day as well.
        TEST(RtxFilmTest, homeOutputPastedIsKeys)
        {
            Rtx::Stop dawn{ .mName = "balmora",
                .mStand = { .mCell = "-3,-2",
                    .mEye = osg::Vec3f(-18075.145f, -17586.46f, 638.41016f),
                    .mLook = osg::Vec3f(-18625.098f, -16765.438f, 485.20374f) },
                .mSky = { .mHour = 6.5f, .mDay = 2, .mWeather = "Overcast" } };
            Rtx::Stop noon = dawn;
            noon.mSky = { .mHour = 12.0f, .mDay = 0, .mWeather = "Clear" };

            const std::vector<FilmKey> keys = read(describeStanding(dawn) + describeStanding(noon) + describeKey(dawn));
            ASSERT_EQ(keys.size(), 3u);

            EXPECT_EQ(keys[0].mName, "balmora");
            EXPECT_EQ(keys[1].mName, "balmora");
            EXPECT_EQ(keys[0].mCell, "-3,-2");
            EXPECT_EQ(keys[0].mEye, *dawn.mStand.mEye);
            EXPECT_EQ(keys[0].mLook, *dawn.mStand.mLook);
            EXPECT_EQ(keys[0].mHour, 6.5f);
            EXPECT_EQ(keys[0].mWeather, "Overcast");
            EXPECT_FALSE(keys[0].mDay.has_value()) << "the block has no day; the command line beside it is a comment";

            EXPECT_EQ(keys[1].mHour, sDefaultHour);
            EXPECT_EQ(keys[1].mWeather, sDefaultWeather);

            EXPECT_EQ(keys[2].mEye, *dawn.mStand.mEye);
            EXPECT_EQ(keys[2].mHour, 6.5f);
            EXPECT_EQ(keys[2].mDay, 2);
            EXPECT_EQ(keys[2].mWeather, "Overcast");

            const std::vector<FilmKey> timed
                = read("[a]\ncell = 0,0\npos = 1,2,3\nlook = 4,5,6\nseconds = 2.5\nhold = 1\ncut = false\n");
            EXPECT_EQ(timed[0].mSeconds, 2.5f);
            EXPECT_EQ(timed[0].mHold, 1.0f);
            EXPECT_EQ(timed[0].mCut, false);
        }

        /// A key misread is a film of somewhere else, so every malformed line is refused by its
        /// number.
        TEST(RtxFilmTest, aMalformedKeyIsRefusedByItsLine)
        {
            const std::string place = "[a]\ncell = 0,0\npos = 1,2,3\nlook = 4,5,6\n";
            EXPECT_EQ(refusal(place + "speed = 3\n"), "tour.keys:5: a key has no field called \"speed\"");
            EXPECT_EQ(
                refusal(place + "hour = 24\n"), "tour.keys:5: hour \"24\" is not from 0 up to but not including 24");
            EXPECT_EQ(refusal(place + "weather = Drizzle\n"),
                "tour.keys:5: weather \"Drizzle\" is none of the weathers the content files name");
            EXPECT_EQ(refusal(place + "seconds = 0\n"), "tour.keys:5: seconds \"0\" is not a length of time");
            EXPECT_EQ(refusal(place + "cut = yes\n"), "tour.keys:5: cut \"yes\" is not true or false");
            EXPECT_EQ(
                refusal(place + "day = -1\n"), "tour.keys:5: day \"-1\" is not a whole number of days from nought");
            EXPECT_EQ(refusal("cell = 0,0\n"), "tour.keys:1: a field comes before the first [key]");
            EXPECT_EQ(refusal("# a comment\n\n[a]\ncell = 0,0\npos = 1,2,3\n[b]\n"),
                "tour.keys:3: key \"a\" names no pos and look");
            EXPECT_EQ(refusal(place + "[b\n"), "tour.keys:5: a section's name is not closed by ]");
            EXPECT_EQ(
                refusal(place + "pos = 1,2\n"), "tour.keys:5: pos \"1,2\" is not three numbers separated by commas")
                << "quoted whole, and not from where the reading stopped";
            EXPECT_EQ(refusal("[a]\ncell = 0,0\npos =\nlook = 1,2,3\n"),
                "tour.keys:3: pos \"\" is not three numbers separated by commas")
                << "an empty point is no point, and not one left unsaid";
            EXPECT_EQ(refusal("# nothing\n"), "tour.keys states no keys");
        }

        TEST(RtxFilmTest, anExteriorIsAPairOfIntegers)
        {
            EXPECT_TRUE(isExteriorCell("-3,-2"));
            EXPECT_TRUE(isExteriorCell(" 2 , -10"));
            EXPECT_FALSE(isExteriorCell("Balmora, Guild of Mages"));
            EXPECT_FALSE(isExteriorCell("-3"));
            EXPECT_FALSE(isExteriorCell("1,2,3"));
            EXPECT_FALSE(isExteriorCell(","));
        }

        /// A key flies from the one before it where both are in one space and near, and cuts
        /// otherwise; a key says so itself where it wants the other.
        TEST(RtxFilmTest, aKeyTooFarOrElsewhereIsACut)
        {
            std::vector<FilmKey> keys{
                keyAt("start", "0,0", osg::Vec3f(0, 0, 0)),
                keyAt("near", "0,0", osg::Vec3f(1000, 0, 0)),
                keyAt("inside", "Balmora, Guild of Mages", osg::Vec3f(0, 0, 0)),
                keyAt("same-room", "Balmora, Guild of Mages", osg::Vec3f(500, 0, 0)),
                keyAt("other-room", "Vivec, Arena", osg::Vec3f(500, 0, 0)),
                keyAt("outside", "2,2", osg::Vec3f(0, 0, 0)),
                keyAt("far", "4,2", osg::Vec3f(20000, 0, 0)),
                keyAt("asked", "4,2", osg::Vec3f(20100, 0, 0)),
                keyAt("forbidden", "9,9", osg::Vec3f(90000, 0, 0)),
            };
            keys[7].mCut = true;
            keys[8].mCut = false;

            const FilmPlan plan = planFilm(keys, pacingForTests());
            ASSERT_EQ(plan.mTakes.size(), 6u);

            const auto take = [&](std::size_t at) { return std::pair(plan.mTakes[at].mFirst, plan.mTakes[at].mEnd); };
            EXPECT_EQ(take(0), (std::pair<std::size_t, std::size_t>(0, 2)));
            EXPECT_EQ(take(1), (std::pair<std::size_t, std::size_t>(2, 4)));
            EXPECT_EQ(take(2), (std::pair<std::size_t, std::size_t>(4, 5)));
            EXPECT_EQ(take(3), (std::pair<std::size_t, std::size_t>(5, 6)));
            EXPECT_EQ(take(4), (std::pair<std::size_t, std::size_t>(6, 7)));
            EXPECT_EQ(take(5), (std::pair<std::size_t, std::size_t>(7, 9)));

            EXPECT_EQ(plan.mTakes[0].mCut, FilmCut::First);
            EXPECT_EQ(plan.mTakes[1].mCut, FilmCut::Indoors);
            EXPECT_EQ(plan.mTakes[2].mCut, FilmCut::Interior);
            EXPECT_EQ(plan.mTakes[3].mCut, FilmCut::Outdoors);
            EXPECT_EQ(plan.mTakes[4].mCut, FilmCut::Distance);
            EXPECT_FLOAT_EQ(plan.mTakes[4].mJump, 20000.0f);
            EXPECT_EQ(plan.mTakes[5].mCut, FilmCut::Asked);
        }

        /// Each change asks for a length, and the longest wins, at ten frames a second: a thousand
        /// units at a hundred a second is ten seconds; a quarter turn one image width, seven; a
        /// pitch of thirty degrees half an image height, 3.5; three hours at two seconds each, six;
        /// a weather crossing, eight; nothing at all, a still of four; and a key's own seconds over
        /// every one of them. A length too short for a frame is one frame.
        TEST(RtxFilmTest, aSegmentTakesTheLongestOfWhatItsChangesAsk)
        {
            const osg::Vec3f spot(0, 0, 0);
            const osg::Vec3f north(0, 1e4f, 0);
            const osg::Vec3f east(1e4f, 0, 0);
            const float climb = 1e4f * std::tan(osg::DegreesToRadians(30.0f));
            const osg::Vec3f up(0, 1e4f, climb);
            const osg::Vec3f eastUp(1e4f, 0, climb);
            const osg::Vec3f there(1000, 0, 0);

            std::vector<FilmKey> keys{
                keyAt("a", "0,0", spot, north),
                keyAt("flown", "0,0", there, north),
                keyAt("turned", "0,0", there, east),
                keyAt("tilted", "0,0", there, eastUp),
                keyAt("later", "0,0", there, eastUp),
                keyAt("rain", "0,0", there, eastUp),
                keyAt("same", "0,0", there, eastUp),
                keyAt("given", "0,0", osg::Vec3f(5000, 0, 0), up),
                keyAt("nudged", "0,0", osg::Vec3f(5004, 0, 0), up),
            };
            keys[3].mHour = 12.0f;
            for (std::size_t at = 4; at < keys.size(); ++at)
                keys[at].mHour = 15.0f;
            for (std::size_t at = 5; at < keys.size(); ++at)
                keys[at].mWeather = "Rain";
            keys[7].mSeconds = 2.5f;

            const FilmPlan plan = planFilm(keys, pacingForTests());
            ASSERT_EQ(plan.mTakes.size(), 1u);
            const std::vector<FilmSegment>& segments = plan.mTakes[0].mSegments;
            ASSERT_EQ(segments.size(), 8u);

            EXPECT_EQ(segments[0].mFrames, 100u);
            EXPECT_EQ(segments[0].mPace, FilmPace::Distance);
            EXPECT_EQ(segments[1].mFrames, 70u);
            EXPECT_EQ(segments[1].mPace, FilmPace::Turn);
            EXPECT_EQ(segments[2].mFrames, 35u);
            EXPECT_EQ(segments[2].mPace, FilmPace::Turn);
            EXPECT_EQ(segments[3].mFrames, 60u);
            EXPECT_EQ(segments[3].mPace, FilmPace::Clock);
            EXPECT_EQ(segments[4].mFrames, 80u);
            EXPECT_EQ(segments[4].mPace, FilmPace::Weather);
            EXPECT_EQ(segments[5].mFrames, 40u);
            EXPECT_EQ(segments[5].mPace, FilmPace::Still);
            EXPECT_EQ(segments[6].mFrames, 25u);
            EXPECT_EQ(segments[6].mPace, FilmPace::Given);
            EXPECT_EQ(segments[7].mFrames, 1u) << "four units is 0.04 s, four tenths of a frame";
            EXPECT_EQ(segments[7].mPace, FilmPace::Distance);

            // Forty units and three hours: the clock's six seconds beat the flight's 0.4.
            std::vector<FilmKey> both{ keyAt("a", "0,0", spot), keyAt("b", "0,0", osg::Vec3f(40, 0, 0)) };
            both[1].mHour = 15.0f;
            EXPECT_EQ(planFilm(both, pacingForTests()).mTakes[0].mSegments[0].mFrames, 60u);
        }

        /// A hold is its key twice, both resting; a take of one key holds it for a still; each take
        /// is numbered on from the one before; and a stop carries the take whole.
        TEST(RtxFilmTest, takesAreLaidEndToEndAndBecomeStops)
        {
            std::vector<FilmKey> keys{
                keyAt("a", "0,0", osg::Vec3f(0, 0, 0)),
                keyAt("b", "0,0", osg::Vec3f(1000, 0, 0)),
                keyAt("c", "0,0", osg::Vec3f(2000, 0, 0)),
                keyAt("room", "Vivec, Arena", osg::Vec3f(0, 0, 0)),
            };
            keys[1].mHold = 1.5f;
            keys[3].mNote = "the arena";
            keys[3].mDay = 5;

            const FilmPlan plan = planFilm(keys, pacingForTests());
            ASSERT_EQ(plan.mTakes.size(), 2u);

            const std::vector<Rtx::TrackKey>& track = plan.mTakes[0].mTrack;
            ASSERT_EQ(track.size(), 4u);
            EXPECT_EQ(track[0].mFrame, 0u);
            EXPECT_EQ(track[1].mFrame, 100u);
            EXPECT_TRUE(track[1].mRests);
            EXPECT_EQ(track[2].mFrame, 115u);
            EXPECT_TRUE(track[2].mRests);
            EXPECT_EQ(track[2].mEye, track[1].mEye);
            EXPECT_EQ(track[3].mFrame, 215u);
            EXPECT_EQ(plan.mTakes[0].getFrames(), 216u);

            const std::vector<Rtx::TrackKey>& still = plan.mTakes[1].mTrack;
            ASSERT_EQ(still.size(), 2u);
            EXPECT_EQ(still[1].mFrame, 40u) << "the still's four seconds";
            EXPECT_EQ(plan.mTakes[1].mFirstFrame, 216u);
            EXPECT_EQ(plan.getFrames(), 216u + 41u);

            const std::vector<Rtx::Stop> stops = stopsFor(plan, "film/frames");
            ASSERT_EQ(stops.size(), 2u);

            const Rtx::Stop& room = stops[1];
            EXPECT_EQ(room.mName, "take-2-room");
            EXPECT_EQ(room.mNote, "the arena");
            EXPECT_EQ(room.mStand.mCell, "Vivec, Arena");
            EXPECT_EQ(room.mStand.mEye, keys[3].mEye);
            EXPECT_EQ(room.mSky.mHour, sDefaultHour);
            EXPECT_EQ(room.mSky.mDay, 5);
            EXPECT_EQ(room.mSky.mWeather, sDefaultWeather);
            EXPECT_EQ(room.mSchedule.mSpec.getWarmup(0.1f), 20u) << "two seconds at ten frames a second";
            EXPECT_EQ(room.mSchedule.mSpec.getMeasured(0.1f), 41u);
            ASSERT_TRUE(room.mSchedule.mTrack.has_value());
            EXPECT_EQ(room.mSchedule.mTrack->getFrames(), 41u);
            ASSERT_TRUE(room.mActions.mFilm.has_value());
            EXPECT_EQ(room.mActions.mFilm->mFirst, 216u);
            EXPECT_EQ(room.mActions.mFilm->mDirectory, std::filesystem::path("film/frames"));

            EXPECT_EQ(stops[0].mSky.mDay, 0) << "the command line's day where a key names none";
            EXPECT_EQ(stops[0].mSchedule.mTrack->pose(100).mEye, keys[1].mEye);
            EXPECT_EQ(stops[0].mSchedule.mTrack->pose(110).mEye, keys[1].mEye) << "held";
        }

        /// The plan a person reads before an hour of rendering.
        TEST(RtxFilmTest, thePlanSaysEveryLengthAndWhy)
        {
            std::vector<FilmKey> keys{
                keyAt("dock", "-2,-9", osg::Vec3f(0, 0, 0)),
                keyAt("shore", "-2,-9", osg::Vec3f(1000, 0, 0)),
                keyAt("room", "Vivec, Arena", osg::Vec3f(0, 0, 0)),
            };
            keys[1].mHour = 18.0f;
            keys[1].mWeather = "Rain";

            EXPECT_EQ(describePlan(planFilm(keys, pacingForTests())),
                "film: 3 keys, 2 takes, 162 frames, 16.2 s at 10 frames a second\n"
                "\n"
                "take 1, from frame 0: 12.1 s, cut in: the first key\n"
                "  dock                         -2,-9 12:00, Clear\n"
                "  -> shore                       12.0 s  18:00 Rain, -2,-9  (6.00 hours of clock)\n"
                "\n"
                "take 2, from frame 121: 4.1 s, cut in: outside to inside\n"
                "  room                         Vivec, Arena 12:00, Clear\n");
        }

        /// Only what the film writes goes: six digits and `.png`.
        TEST(RtxFilmTest, clearingTheFramesLeavesEverythingElse)
        {
            const std::filesystem::path frames = TestingOpenMW::outputDirPath("film-frames");
            std::filesystem::remove_all(frames);
            std::filesystem::create_directories(frames);
            for (const char* name :
                { "000000.png", "000001.png", "notes.txt", "12345.png", "0000001.png", "00000a.png" })
                std::ofstream(frames / name) << "x";

            EXPECT_EQ(clearFrames(frames), 2u);
            EXPECT_FALSE(std::filesystem::exists(frames / "000000.png"));
            EXPECT_FALSE(std::filesystem::exists(frames / "000001.png"));
            EXPECT_TRUE(std::filesystem::exists(frames / "notes.txt"));
            EXPECT_TRUE(std::filesystem::exists(frames / "12345.png"));
            EXPECT_TRUE(std::filesystem::exists(frames / "0000001.png"));
            EXPECT_TRUE(std::filesystem::exists(frames / "00000a.png"));

            EXPECT_EQ(clearFrames(frames / "nowhere"), 0u);
            EXPECT_EQ(frameName(42), "000042.png");
        }

#if !defined(_WIN32)
        /// Each path is one word to the shell, a quote inside one included.
        TEST(RtxFilmTest, theEncoderIsHandedEachPathAsOneWord)
        {
            EXPECT_EQ(encodeCommand("/tmp/film/frames", "/tmp/it's here/tour.mp4", 60.0f),
                "ffmpeg -hide_banner -loglevel warning -y -framerate 60 -i '/tmp/film/frames/%06d.png' "
                "-vf \"pad=ceil(iw/2)*2:ceil(ih/2)*2\" -c:v libx264 -preset slow -crf 18 -pix_fmt yuv420p "
                "-movflags +faststart '/tmp/it'\\''s here/tour.mp4'");
        }
#endif
    }
}
