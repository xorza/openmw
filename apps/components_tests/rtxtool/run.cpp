#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Vec3f>

#include <apps/rtxtool/run.hpp>

#include "../rtx/harness.hpp"

namespace RtxTool
{
    namespace
    {
        Rtx::Stop makeSpot()
        {
            return Rtx::Stop{
                .mName = "balmora-mages-guild",
                .mNote = "a guild interior, dense with clutter",
                .mStand = { .mCell = "Balmora, Guild of Mages",
                    .mEye = osg::Vec3f(-283.29843f, -671.29584f, -580.77014f),
                    .mLook = osg::Vec3f(503.60007f, -1265.436f, -747.46844f) },
                .mSky = { .mHour = 12.0f, .mDay = 0, .mWeather = "Clear" },
            };
        }

        TEST(RtxViewpointTest, aSpotSaysWhichWayItFaces)
        {
            const auto facing = [](float x, float y, float z) {
                return Rtx::Stand{ .mEye = osg::Vec3f(), .mLook = osg::Vec3f(x, y, z) };
            };

            // Due north, and the one case the swapped `atan2` also gets right.
            EXPECT_NEAR(bearingOf(facing(0.0f, 100.0f, 0.0f)), 0.0f, 1e-3f);
            EXPECT_NEAR(bearingOf(facing(100.0f, 100.0f, 0.0f)), 45.0f, 1e-3f) << "north-east";
            EXPECT_NEAR(bearingOf(facing(100.0f, 0.0f, 0.0f)), 90.0f, 1e-3f) << "due east";
            // Wrapped rather than negative: a compass has no -90.
            EXPECT_NEAR(bearingOf(facing(-100.0f, 0.0f, 0.0f)), 270.0f, 1e-3f) << "due west";

            // Equal parts along and up is forty-five degrees, and the sign is up rather than down.
            EXPECT_NEAR(climbOf(facing(0.0f, 100.0f, 100.0f)), 45.0f, 1e-3f);
            EXPECT_NEAR(climbOf(facing(0.0f, 100.0f, 0.0f)), 0.0f, 1e-3f);
            EXPECT_NEAR(climbOf(facing(0.0f, 100.0f, -100.0f)), -45.0f, 1e-3f);
            // Straight down, where the horizontal part is zero and `asin` is handed exactly -1.
            EXPECT_NEAR(climbOf(facing(0.0f, 0.0f, -100.0f)), -90.0f, 1e-3f);
        }

        /// The readable line, which is the one nobody parses and everybody reads.
        TEST(RtxViewpointTest, theSpotLineSaysWhereAndWhen)
        {
            const std::string line = describeSpot(makeSpot());
            EXPECT_EQ(line,
                "# Balmora, Guild of Mages at -283, -671, -581 \u2014 bearing 127\u00b0, climb -10\u00b0 \u2014 day 0, "
                "12:00, "
                "Clear\n");

            // A quarter past five in the evening, because a decimal hour is not a time anyone reads.
            Rtx::Stop evening = makeSpot();
            evening.mSky.mHour = 17.25f;
            evening.mSky.mWeather = "Ashstorm";
            EXPECT_NE(describeSpot(evening).find("17:15, Ashstorm"), std::string::npos) << describeSpot(evening);
        }

        /// **The symmetry, asserted rather than assumed**: what the window prints, the view file
        /// reads, and what comes back is the camera that was standing there.
        ///
        /// **Everything P writes, and not only the block half of it.** The readable line goes in
        /// front of it and would be pasted with it, so if the view file could not take a comment the
        /// output would be one edit away from usable rather than usable.
        ///
        /// Exact equality on the floats and not a tolerance — the block is written shortest
        /// round-trip precisely so that a saved viewpoint is the viewpoint, and a position rounded
        /// to the unit is a different frame when the camera is a hand's width from a wall.
        TEST(RtxViewpointTest, theBlockItPrintsIsTheBlockTheViewFileReads)
        {
            const Rtx::Stop spot = makeSpot();

            const std::filesystem::path file = std::filesystem::temp_directory_path() / "openmw-rtx-viewpoint-test.cfg";
            {
                std::ofstream out(file);
                out << describeSpot(spot) << describeBlock(spot);
            }

            const std::vector<Rtx::Stop> read = loadViews(file);
            std::filesystem::remove(file);

            ASSERT_EQ(read.size(), 1u);
            EXPECT_EQ(read.front().mName, spot.mName);
            EXPECT_EQ(read.front().mNote, spot.mNote);
            EXPECT_EQ(read.front().mStand.mCell, spot.mStand.mCell);
            ASSERT_TRUE(read.front().mStand.mEye.has_value());
            ASSERT_TRUE(read.front().mStand.mLook.has_value());
            EXPECT_EQ(*read.front().mStand.mEye, *spot.mStand.mEye);
            EXPECT_EQ(*read.front().mStand.mLook, *spot.mStand.mLook);

            // **Clear noon writes neither condition**, so a view pasted from an ordinary window is
            // still free to be measured under whatever a run names.
            EXPECT_FALSE(read.front().mSky.mHour.has_value()) << describeBlock(spot);
            EXPECT_FALSE(read.front().mSky.mWeather.has_value()) << describeBlock(spot);

            // **And anything else writes both**, because the light is most of what the frame is: a
            // block pasted from a window flown at dawn in a storm has to bring both with it.
            Rtx::Stop dawn = spot;
            dawn.mSky.mHour = 6.5f;
            dawn.mSky.mWeather = "Thunderstorm";

            const std::filesystem::path second
                = std::filesystem::temp_directory_path() / "openmw-rtx-viewpoint-dawn.cfg";
            {
                std::ofstream out(second);
                out << describeSpot(dawn) << describeBlock(dawn);
            }

            const std::vector<Rtx::Stop> back = loadViews(second);
            std::filesystem::remove(second);

            ASSERT_EQ(back.size(), 1u);
            ASSERT_TRUE(back.front().mSky.mHour.has_value());
            EXPECT_EQ(*back.front().mSky.mHour, 6.5f);
            ASSERT_TRUE(back.front().mSky.mWeather.has_value());
            EXPECT_EQ(*back.front().mSky.mWeather, "Thunderstorm");
        }

        /// A window opened by `--cell` has no view to replace: `stopFor` names the stop after the
        /// cell, and the block opens under a slug of that.
        ///
        /// It still has to load: an id the file cannot take, or a missing note line that the parser
        /// treats as a missing field, would make the printed block unpasteable in exactly the case
        /// where there is nothing to paste over.
        TEST(RtxViewpointTest, aWindowOpenedWithoutAViewStillPrintsOne)
        {
            Rtx::Stop bare = makeSpot();
            bare.mName.clear();
            bare.mNote.clear();
            const Rtx::Stop spot = stopFor(bare, std::nullopt, std::nullopt, 0);

            const std::string block = describeBlock(spot);
            EXPECT_EQ(block.find("[balmora-guild-of-mages]"), 0u) << block;
            EXPECT_EQ(block.find("note ="), std::string::npos) << "an empty note is left out, not written blank";

            const std::filesystem::path file
                = std::filesystem::temp_directory_path() / "openmw-rtx-viewpoint-unnamed.cfg";
            {
                std::ofstream out(file);
                out << describeSpot(spot) << block;
            }

            const std::vector<Rtx::Stop> read = loadViews(file);
            std::filesystem::remove(file);

            ASSERT_EQ(read.size(), 1u);
            EXPECT_EQ(read.front().mName, "balmora-guild-of-mages");
            EXPECT_EQ(read.front().mNote, "");
            EXPECT_EQ(read.front().mStand.mCell, spot.mStand.mCell);
        }
    }

    namespace
    {
        /// Where the resource files the tool reads are copied to.
        std::filesystem::path resources()
        {
            return Rtx::Testing::getShaderDirectory().parent_path();
        }

        TEST(RtxBenchSuiteTest, aSuiteFileIsSectionsOfViewNames)
        {
            const std::filesystem::path file = std::filesystem::temp_directory_path() / "openmw-rtx-suite-test.cfg";
            {
                std::ofstream written(file);
                written << "[quick]\n"
                           "note = two of them\n"
                           "views = balmora, vivec\n"
                           "\n"
                           "[one]\n"
                           "views = arkngthand\n";
            }

            const std::vector<BenchSuite> suites = loadSuites(file);
            ASSERT_EQ(suites.size(), 2u);

            const BenchSuite* quick = findSuite(suites, "quick");
            ASSERT_NE(quick, nullptr);
            EXPECT_EQ(quick->mNote, "two of them");
            EXPECT_EQ(quick->mViews, (std::vector<std::string>{ "balmora", "vivec" }));

            const BenchSuite* one = findSuite(suites, "one");
            ASSERT_NE(one, nullptr);
            EXPECT_EQ(one->mViews, (std::vector<std::string>{ "arkngthand" }));
            EXPECT_TRUE(one->mNote.empty()) << "a note is optional";

            EXPECT_EQ(findSuite(suites, "nothing"), nullptr);

            std::filesystem::remove(file);
        }

        /// A suite with no views in it, and a field nobody defined.
        ///
        /// **Both throw rather than being skipped.** A profiling run costs minutes, and the two ways
        /// to waste them are a suite that silently runs nothing and a typo that silently drops a
        /// place out of the list.
        TEST(RtxBenchSuiteTest, aMalformedSuiteSaysSoRatherThanRunningNothing)
        {
            const std::filesystem::path file = std::filesystem::temp_directory_path() / "openmw-rtx-suite-bad.cfg";
            {
                std::ofstream written(file);
                written << "[empty]\nnote = nothing here\n";
            }
            EXPECT_THROW(loadSuites(file), std::runtime_error) << "a suite naming no views";

            {
                std::ofstream written(file);
                written << "[typo]\nveiws = balmora\n";
            }
            EXPECT_THROW(loadSuites(file), std::runtime_error) << "a field nobody defined";

            std::filesystem::remove(file);
            EXPECT_THROW(loadSuites(file), std::runtime_error) << "a file that is not there";
        }

        /// Every place the shipped suites name is a place the shipped views file has.
        ///
        /// **The one way this pair can be wrong that nothing else catches.** A view renamed in
        /// `views.cfg` leaves `benches.cfg` naming something that no longer exists, and the run that
        /// finds out is the one somebody started and walked away from.
        TEST(RtxBenchSuiteTest, everySuiteNamesViewsThatExist)
        {
            const std::vector<Rtx::Stop> views = loadViews(resources() / "views.cfg");
            const std::vector<BenchSuite> suites = loadSuites(resources() / "benches.cfg");

            EXPECT_NE(findSuite(suites, "default"), nullptr) << "`bench` with no arguments runs [default]";

            for (const BenchSuite& suite : suites)
                for (const std::string& name : suite.mViews)
                    EXPECT_NE(findView(views, name), nullptr)
                        << "suite \"" << suite.mName << "\" names \"" << name << "\", which views.cfg has not got";
        }
    }

    namespace
    {
        /// Writes `text` to a scratch view file, reads it back, and removes the file.
        std::vector<Rtx::Stop> readViews(std::string_view text)
        {
            const std::filesystem::path file = std::filesystem::temp_directory_path() / "openmw-rtx-route-test.cfg";
            {
                std::ofstream out(file);
                out << text;
            }

            struct Remove
            {
                std::filesystem::path mFile;
                ~Remove() { std::filesystem::remove(mFile); }
            } removed{ file };

            return loadViews(file);
        }

        /// A route resolves to the coordinates of the view it names, and both halves are required.
        ///
        /// **The destination is copied at load and never looked up again**, so this is the one place
        /// that can get the pairing wrong — and getting it wrong flies the camera somewhere else,
        /// which a benchmark reports as a different number rather than as an error.
        TEST(RtxViewsTest, aRouteTakesItsDestinationFromTheViewItNames)
        {
            const std::vector<Rtx::Stop> read = readViews(R"(
[start]
cell = -3,-2
pos = 100, 200, 300
look = 100, 300, 300
to = finish
speed = 1500

[finish]
cell = -1,-2
pos = 8292, 200, 700
look = 8292, 300, 700
)");

            ASSERT_EQ(read.size(), std::size_t{ 2 });
            const Rtx::Stop* start = findView(read, "start");
            ASSERT_NE(start, nullptr);
            ASSERT_TRUE(start->mSchedule.mRoute.has_value());

            // The destination view is resolved when the file is read, so what a route carries is
            // where it ends rather than the name of a place to look up later.
            EXPECT_EQ(start->mSchedule.mRoute->mTo, osg::Vec3f(8292.0f, 200.0f, 700.0f));
            EXPECT_EQ(start->mSchedule.mRoute->mLookTo, osg::Vec3f(8292.0f, 300.0f, 700.0f));
            EXPECT_EQ(start->mSchedule.mRoute->mSpeed, 1500.0f);

            // The destination is an ordinary view and goes nowhere itself.
            const Rtx::Stop* finish = findView(read, "finish");
            ASSERT_NE(finish, nullptr);
            EXPECT_FALSE(finish->mSchedule.mRoute.has_value());
        }

        /// Every way of half-writing a route is a refusal rather than a camera that stands still.
        TEST(RtxViewsTest, aHalfWrittenRouteIsRefused)
        {
            constexpr std::string_view sEnd = "\n[finish]\ncell = -1,-2\npos = 8292, 0, 0\nlook = 8292, 100, 0\n";

            EXPECT_THROW(
                readViews(std::string("[start]\ncell = -3,-2\nto = finish\n") + std::string(sEnd)), std::runtime_error)
                << "a destination with no speed";

            EXPECT_THROW(readViews("[start]\ncell = -3,-2\nspeed = 1500\n"), std::runtime_error)
                << "a speed with nowhere to go";

            EXPECT_THROW(readViews("[start]\ncell = -3,-2\nto = nowhere\nspeed = 1500\n"), std::runtime_error)
                << "a destination that is not a view";

            EXPECT_THROW(readViews("[start]\ncell = -3,-2\nto = finish\nspeed = 1500\n\n[finish]\ncell = -1,-2\n"),
                std::runtime_error)
                << "a destination with no coordinates of its own to arrive at";

            EXPECT_THROW(readViews(std::string("[start]\ncell = -3,-2\nto = finish\nspeed = -1\n") + std::string(sEnd)),
                std::runtime_error)
                << "a speed that goes backwards";

            EXPECT_THROW(
                readViews(std::string("[start]\ncell = -3,-2\nto = finish\nspeed = quickly\n") + std::string(sEnd)),
                std::runtime_error)
                << "a speed that is not a number";
        }

        /// A place says what it is looked at under, and takes where it stands from another place.
        ///
        /// **The pair is the point.** A dawn row and a noon row of one camera mean something beside
        /// each other only where the camera is identical by construction — coordinates copied by
        /// hand drift the first time either is moved, and the pair then reads two cameras as a
        /// difference the hour made.
        TEST(RtxViewsTest, aPlaceFixesItsConditionsAndTakesItsCameraFromWhatItIsLike)
        {
            const std::vector<Rtx::Stop> read = readViews(R"(
[ship]
cell = -2,-9
pos = 100, 200, 300
look = 100, 300, 300

[ship-dawn]
like = ship
hour = 6.5

[ship-overcast]
like = ship
weather = Overcast

[ship-dusk-from-the-mast]
like = ship
pos = 100, 200, 900
hour = 19.25
)");

            ASSERT_EQ(read.size(), std::size_t{ 4 });

            // A place that fixes nothing keeps both conditions absent, which is what lets a run name
            // them.
            const Rtx::Stop* noon = findView(read, "ship");
            ASSERT_NE(noon, nullptr);
            EXPECT_FALSE(noon->mSky.mHour.has_value());
            EXPECT_FALSE(noon->mSky.mWeather.has_value());

            const Rtx::Stop* dawn = findView(read, "ship-dawn");
            ASSERT_NE(dawn, nullptr);
            ASSERT_TRUE(dawn->mSky.mHour.has_value());
            EXPECT_EQ(*dawn->mSky.mHour, 6.5f);

            // **The two conditions are independent**: an hour fixed leaves the sky free and a sky
            // fixed leaves the hour free, so a place may name either alone.
            EXPECT_FALSE(dawn->mSky.mWeather.has_value());

            const Rtx::Stop* overcast = findView(read, "ship-overcast");
            ASSERT_NE(overcast, nullptr);
            ASSERT_TRUE(overcast->mSky.mWeather.has_value());
            ASSERT_TRUE(overcast->mStand.mEye.has_value());
            EXPECT_EQ(*overcast->mSky.mWeather, "Overcast");
            EXPECT_FALSE(overcast->mSky.mHour.has_value());
            EXPECT_EQ(*overcast->mStand.mEye, osg::Vec3f(100.0f, 200.0f, 300.0f));

            // The whole camera, taken rather than restated.
            EXPECT_EQ(dawn->mStand.mCell, "-2,-9");
            ASSERT_TRUE(dawn->mStand.mEye.has_value());
            ASSERT_TRUE(dawn->mStand.mLook.has_value());
            EXPECT_EQ(*dawn->mStand.mEye, osg::Vec3f(100.0f, 200.0f, 300.0f));
            EXPECT_EQ(*dawn->mStand.mLook, osg::Vec3f(100.0f, 300.0f, 300.0f));

            // **What a borrower states itself is kept**, so a place may sit somewhere else under
            // the same cell and the same view of it.
            const Rtx::Stop* mast = findView(read, "ship-dusk-from-the-mast");
            ASSERT_NE(mast, nullptr);
            ASSERT_TRUE(mast->mStand.mEye.has_value());
            EXPECT_EQ(*mast->mStand.mEye, osg::Vec3f(100.0f, 200.0f, 900.0f)) << "its own position was overwritten";
            EXPECT_EQ(*mast->mStand.mLook, osg::Vec3f(100.0f, 300.0f, 300.0f)) << "the look it did not state";
            EXPECT_EQ(mast->mStand.mCell, "-2,-9");
        }

        /// Every way of writing a condition or a likeness wrong is a refusal.
        ///
        /// A view that quietly stood at another hour, under another sky, or in another place would
        /// report a number against a frame nobody asked for — which is the failure this whole file
        /// exists to stop.
        TEST(RtxViewsTest, aConditionOrALikenessThatCannotBeMeantIsRefused)
        {
            constexpr std::string_view sShip = "[ship]\ncell = -2,-9\npos = 1, 2, 3\nlook = 1, 9, 3\n";

            EXPECT_THROW(readViews(std::string(sShip) + "[dawn]\nlike = ship\nhour = dawn\n"), std::runtime_error)
                << "an hour that is not a number";

            // **The whole of the field, so a number with a letter after it is a typo and not an
            // hour.** A view that stood at six because `6h` began with a six would report a figure
            // against a frame nobody asked for.
            EXPECT_THROW(readViews(std::string(sShip) + "[dawn]\nlike = ship\nhour = 6h\n"), std::runtime_error)
                << "an hour with a letter after it";

            EXPECT_THROW(readViews(std::string(sShip) + "[dawn]\nlike = ship\nhour = 24\n"), std::runtime_error)
                << "an hour off the end of the clock";

            EXPECT_THROW(readViews(std::string(sShip) + "[dawn]\nlike = ship\nhour = -1\n"), std::runtime_error)
                << "an hour before the day began";

            // Midnight and a moment before the next one are both hours of the day.
            EXPECT_NO_THROW(readViews(std::string(sShip) + "[dark]\nlike = ship\nhour = 0\n"));
            EXPECT_NO_THROW(readViews(std::string(sShip) + "[late]\nlike = ship\nhour = 23.99\n"));

            // **A weather is one of the ten and spelled as the content files spell it.** Anything
            // else reaches the fallback map as a key it refuses, which is a throw at the frame
            // rather than at the file — and by then the run has staged a cell for it.
            EXPECT_THROW(readViews(std::string(sShip) + "[grim]\nlike = ship\nweather = Drizzle\n"), std::runtime_error)
                << "a weather that is none of the ten";

            EXPECT_THROW(
                readViews(std::string(sShip) + "[grim]\nlike = ship\nweather = overcast\n"), std::runtime_error)
                << "a weather spelled in the wrong case";

            EXPECT_NO_THROW(readViews(std::string(sShip) + "[grim]\nlike = ship\nweather = Thunderstorm\n"));

            EXPECT_THROW(readViews(std::string(sShip) + "[dawn]\nlike = nowhere\n"), std::runtime_error)
                << "like a view that is not there";

            EXPECT_THROW(readViews("[dawn]\nlike = dawn\n"), std::runtime_error) << "like itself";

            EXPECT_THROW(
                readViews(std::string(sShip) + "[dawn]\nlike = ship\n[later]\nlike = dawn\n"), std::runtime_error)
                << "a chain, which would make the order things are read in decide what a view is";
        }

        /// Which condition wins, which is the one rule every command that draws a view reads.
        ///
        /// **A settled place is what a run stands at, so neither condition is optional on it.** The
        /// file's own entry keeps its optionals, because a listing prints only what a view fixes.
        TEST(RtxViewsTest, theConditionOnTheCommandLineBeatsTheOneAPlaceFixes)
        {
            const Rtx::Stop entry{
                .mName = "dawn-deck",
                .mNote = "a deck at dawn",
                .mStand = { .mCell = "Vivec, Foreign Quarter",
                    .mEye = osg::Vec3f(1.0f, 2.0f, 3.0f),
                    .mLook = osg::Vec3f(4.0f, 5.0f, 6.0f) },
                .mSky = { .mHour = 6.5f, .mWeather = std::string("Overcast") },
                .mSchedule = { .mRoute
                    = Rtx::Route{ .mTo = osg::Vec3f(7.0f, 8.0f, 9.0f), .mLookTo = osg::Vec3f(), .mSpeed = 400.0f } },
            };

            const Rtx::Stop bare{ .mStand = { .mCell = "-2,-9" } };

            // Neither says anything: noon under a clear sky, which is how a picture of a place is
            // taken.
            EXPECT_EQ(stopFor(bare, std::nullopt, std::nullopt, 0).mSky.mHour, sDefaultHour);
            EXPECT_EQ(stopFor(bare, std::nullopt, std::nullopt, 0).mSky.mWeather, sDefaultWeather);

            // Only the place: the place decides, which is what makes a view id one frame.
            EXPECT_EQ(stopFor(entry, std::nullopt, std::nullopt, 0).mSky.mHour, 6.5f);
            EXPECT_EQ(stopFor(entry, std::nullopt, std::nullopt, 0).mSky.mWeather, "Overcast");

            // The command line, over a place that fixes one and over a place that does not.
            EXPECT_EQ(stopFor(entry, 9.0f, std::string("Rain"), 0).mSky.mHour, 9.0f);
            EXPECT_EQ(stopFor(entry, 9.0f, std::string("Rain"), 0).mSky.mWeather, "Rain");
            EXPECT_EQ(stopFor(bare, 9.0f, std::string("Rain"), 0).mSky.mHour, 9.0f);
            EXPECT_EQ(stopFor(bare, 9.0f, std::string("Rain"), 0).mSky.mWeather, "Rain");

            // And the three answers differ, so the rule is doing something.
            EXPECT_NE(stopFor(entry, std::nullopt, std::nullopt, 0).mSky.mHour,
                stopFor(entry, 9.0f, std::nullopt, 0).mSky.mHour);
            EXPECT_NE(stopFor(bare, std::nullopt, std::nullopt, 0).mSky.mHour,
                stopFor(entry, std::nullopt, std::nullopt, 0).mSky.mHour);

            // Everything that is not a condition is the entry's, unchanged.
            const Rtx::Stop settled = stopFor(entry, std::nullopt, std::nullopt, 3);
            EXPECT_EQ(settled.mName, "dawn-deck");
            EXPECT_EQ(settled.mNote, "a deck at dawn");
            EXPECT_EQ(settled.mStand.mCell, "Vivec, Foreign Quarter");
            EXPECT_EQ(settled.mStand.mEye, entry.mStand.mEye);
            EXPECT_EQ(settled.mStand.mLook, entry.mStand.mLook);
            EXPECT_EQ(settled.mSky.mDay, 3);
            ASSERT_TRUE(settled.mSchedule.mRoute.has_value());
            EXPECT_EQ(settled.mSchedule.mRoute->mSpeed, 400.0f);

            // **A view with no id of its own is named after its cell**, because a report row and a
            // hash file are keyed on the name and neither can be keyed on nothing.
            EXPECT_EQ(stopFor(bare, std::nullopt, std::nullopt, 0).mName, "-2,-9");
        }
    }
}
