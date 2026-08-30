#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Vec3f>

#include <components/rtx/renderer.hpp>

#include <apps/rtxtool/placement.hpp>
#include <apps/rtxtool/view.hpp>
#include <apps/rtxtool/viewpoint.hpp>
#include <apps/rtxtool/views.hpp>

namespace RtxTool
{
    namespace
    {
        ViewRequest makeRequest()
        {
            ViewRequest request;
            request.mCell = "Balmora, Guild of Fighters";
            request.mFieldOfView = 60.0f;
            request.mWeather = "Ashstorm";
            request.mHour = 17.25f;
            request.mFilter = false;
            return request;
        }

        /// A held exposure names its number where a measured one names itself.
        ///
        /// **Both are in the line because both change the frame**, and one of them changes what it
        /// costs: measuring is two dispatches over the finished image, about a tenth of a
        /// millisecond at 4K.
        TEST(RtxProfileLineTest, aHeldExposureIsInTheLineAsItsNumber)
        {
            ViewRequest request = makeRequest();
            const Rtx::ValidationOptions validation{};
            const osg::Vec3f origin(0.0f, 0.0f, 0.0f);
            const osg::Vec3f target(0.0f, 100.0f, 0.0f);

            EXPECT_NE(describeProfile(request, validation, origin, target, 64, 64).find("--exposure=auto"),
                std::string::npos);

            request.mExposure = 0.25f;
            EXPECT_NE(describeProfile(request, validation, origin, target, 64, 64).find("--exposure=0.25"),
                std::string::npos);
        }

        /// A profiling line has to be pasteable and it has to be exact, so both are asserted.
        ///
        /// The position is deliberately one that rounding would lose, and it survives because the
        /// formatting is shortest-round-trip rather than fixed. The hour avoids sunrise and sunset
        /// so that this line is one that actually runs.
        TEST(RtxProfileLineTest, everyConditionTheFrameDependsOnIsInTheLine)
        {
            const ViewRequest request = makeRequest();
            const Rtx::ValidationOptions validation{
                .mEnabled = true, .mSynchronization = true, .mGpuAssisted = false
            };
            const osg::Vec3f origin(-19216.5f, -14896.25f, 160.0f);
            const osg::Vec3f target(-19323.0f, -13903.0f, 109.5f);

            EXPECT_EQ(describeProfile(request, validation, origin, target, 2560, 1440),
                "--cell=\"Balmora, Guild of Fighters\" --pos=-19216.5,-14896.25,160 --look=-19323,-13903,109.5"
                " --fov=60 --size=2560x1440 --weather=Ashstorm --hour=17.25 --day=0 --exposure=auto"
                " --upscale=off --preset=d --filter=false --validation=true"
                " --sync-validation=true --gpu-validation=false");
        }

        /// What the line is for: the tool's own parser reading it back to the same floats.
        TEST(RtxProfileLineTest, thePositionItPrintsIsThePositionItParsesBack)
        {
            const osg::Vec3f origin(-19216.5f, -14896.25f, 160.0f);
            const std::string line
                = describeProfile(makeRequest(), Rtx::ValidationOptions{}, origin, osg::Vec3f(1.0f, 2.0f, 3.0f), 8, 8);

            const std::size_t start = line.find("--pos=") + 6;
            const std::string printed = line.substr(start, line.find(' ', start) - start);

            const std::optional<osg::Vec3f> parsed = parseVec3(printed, "--pos");
            ASSERT_TRUE(parsed.has_value());
            EXPECT_EQ(*parsed, origin);
        }

        /// Each of the fields that is a flag rather than a value, since a line that dropped one
        /// would reproduce a different frame — or the same frame at a different price — while
        /// looking correct.
        TEST(RtxProfileLineTest, everySwitchThatChangesTheFrameChangesTheLine)
        {
            ViewRequest shaded = makeRequest();
            ViewRequest albedo = makeRequest();
            albedo.mShowAlbedo = true;

            const osg::Vec3f at(0.0f, 0.0f, 0.0f);
            const osg::Vec3f to(0.0f, 1.0f, 0.0f);
            const Rtx::ValidationOptions off{};
            const Rtx::ValidationOptions gpu{ .mEnabled = true, .mGpuAssisted = true };

            ViewRequest denoised = makeRequest();
            denoised.mFilter = true;

            EXPECT_NE(describeProfile(shaded, off, at, to, 8, 8), describeProfile(albedo, off, at, to, 8, 8));
            EXPECT_NE(describeProfile(shaded, off, at, to, 8, 8), describeProfile(denoised, off, at, to, 8, 8));
            EXPECT_NE(describeProfile(shaded, off, at, to, 8, 8), describeProfile(shaded, gpu, at, to, 8, 8));
            EXPECT_TRUE(describeProfile(albedo, off, at, to, 8, 8).ends_with(" --albedo"));

            // **The two the line used to leave out.** `shot` upscales at quality unless told
            // otherwise, so a window flown with neither of these named profiled into a command that
            // rendered at another mode through another network — the same line, a different frame,
            // and a different price.
            ViewRequest upscaled = makeRequest();
            upscaled.mUpscale = Rtx::Upscale::Performance;
            ViewRequest latest = makeRequest();
            latest.mPreset = Rtx::Preset::E;

            EXPECT_NE(describeProfile(shaded, off, at, to, 8, 8), describeProfile(upscaled, off, at, to, 8, 8));
            EXPECT_NE(describeProfile(shaded, off, at, to, 8, 8), describeProfile(latest, off, at, to, 8, 8));
        }

        Viewpoint makeSpot()
        {
            return Viewpoint{
                .mView = "balmora-mages-guild",
                .mNote = "a guild interior, dense with clutter",
                .mCell = "Balmora, Guild of Mages",
                .mOrigin = osg::Vec3f(-283.29843f, -671.29584f, -580.77014f),
                .mTarget = osg::Vec3f(503.60007f, -1265.436f, -747.46844f),
                .mWeather = "Clear",
                .mHour = 12.0f,
            };
        }

        /// Where the camera points, in the two numbers a person can hold in their head.
        ///
        /// **North is +Y and east is +X**, which is Morrowind's convention and not a maths library's
        /// — so the arguments to `atan2` come the other way round, and getting them the usual way
        /// round mirrors every bearing about north-east. Each case below is a direction whose answer
        /// is exact rather than approximate, and the four of them disagree under that swap.
        TEST(RtxViewpointTest, aSpotSaysWhichWayItFaces)
        {
            const auto facing = [](float x, float y, float z) {
                Viewpoint spot;
                spot.mOrigin = osg::Vec3f();
                spot.mTarget = osg::Vec3f(x, y, z);
                return spot;
            };

            // Due north, and the one case the swapped `atan2` also gets right.
            EXPECT_NEAR(facing(0.0f, 100.0f, 0.0f).getBearing(), 0.0f, 1e-3f);
            EXPECT_NEAR(facing(100.0f, 100.0f, 0.0f).getBearing(), 45.0f, 1e-3f) << "north-east";
            EXPECT_NEAR(facing(100.0f, 0.0f, 0.0f).getBearing(), 90.0f, 1e-3f) << "due east";
            // Wrapped rather than negative: a compass has no -90.
            EXPECT_NEAR(facing(-100.0f, 0.0f, 0.0f).getBearing(), 270.0f, 1e-3f) << "due west";

            // Equal parts along and up is forty-five degrees, and the sign is up rather than down.
            EXPECT_NEAR(facing(0.0f, 100.0f, 100.0f).getClimb(), 45.0f, 1e-3f);
            EXPECT_NEAR(facing(0.0f, 100.0f, 0.0f).getClimb(), 0.0f, 1e-3f);
            EXPECT_NEAR(facing(0.0f, 100.0f, -100.0f).getClimb(), -45.0f, 1e-3f);
            // Straight down, where the horizontal part is zero and `asin` is handed exactly -1.
            EXPECT_NEAR(facing(0.0f, 0.0f, -100.0f).getClimb(), -90.0f, 1e-3f);
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
            Viewpoint evening = makeSpot();
            evening.mHour = 17.25f;
            evening.mWeather = "Ashstorm";
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
            const Viewpoint spot = makeSpot();

            const std::filesystem::path file = std::filesystem::temp_directory_path() / "openmw-rtx-viewpoint-test.cfg";
            {
                std::ofstream out(file);
                out << describeSpot(spot) << describeBlock(spot);
            }

            const std::vector<View> read = loadViews(file);
            std::filesystem::remove(file);

            ASSERT_EQ(read.size(), 1u);
            EXPECT_EQ(read.front().mName, spot.mView);
            EXPECT_EQ(read.front().mNote, spot.mNote);
            EXPECT_EQ(read.front().mCell, spot.mCell);
            ASSERT_TRUE(read.front().mOrigin.has_value());
            ASSERT_TRUE(read.front().mTarget.has_value());
            EXPECT_EQ(*read.front().mOrigin, spot.mOrigin);
            EXPECT_EQ(*read.front().mTarget, spot.mTarget);

            // **Noon writes no hour**, so a view pasted from an ordinary window is still free to be
            // measured at whatever hour a run names.
            EXPECT_FALSE(read.front().mHour.has_value()) << describeBlock(spot);

            // **And any other hour writes one**, because the light is most of what the frame is: a
            // block pasted from a window flown at dawn has to bring the dawn with it.
            Viewpoint dawn = spot;
            dawn.mHour = 6.5f;

            const std::filesystem::path second
                = std::filesystem::temp_directory_path() / "openmw-rtx-viewpoint-dawn.cfg";
            {
                std::ofstream out(second);
                out << describeSpot(dawn) << describeBlock(dawn);
            }

            const std::vector<View> back = loadViews(second);
            std::filesystem::remove(second);

            ASSERT_EQ(back.size(), 1u);
            ASSERT_TRUE(back.front().mHour.has_value());
            EXPECT_EQ(*back.front().mHour, 6.5f);
        }

        /// A window opened by `--cell` has no view to replace, so the block names one after the cell.
        ///
        /// It still has to load: an id the file cannot take, or a missing note line that the parser
        /// treats as a missing field, would make the printed block unpasteable in exactly the case
        /// where there is nothing to paste over.
        TEST(RtxViewpointTest, aWindowOpenedWithoutAViewStillPrintsOne)
        {
            Viewpoint spot = makeSpot();
            spot.mView.clear();
            spot.mNote.clear();

            const std::string block = describeBlock(spot);
            EXPECT_EQ(block.find("[balmora-guild-of-mages]"), 0u) << block;
            EXPECT_EQ(block.find("note ="), std::string::npos) << "an empty note is left out, not written blank";

            const std::filesystem::path file
                = std::filesystem::temp_directory_path() / "openmw-rtx-viewpoint-unnamed.cfg";
            {
                std::ofstream out(file);
                out << describeSpot(spot) << block;
            }

            const std::vector<View> read = loadViews(file);
            std::filesystem::remove(file);

            ASSERT_EQ(read.size(), 1u);
            EXPECT_EQ(read.front().mName, "balmora-guild-of-mages");
            EXPECT_EQ(read.front().mNote, "");
            EXPECT_EQ(read.front().mCell, spot.mCell);
        }

        /// Writes `text` to a scratch view file, reads it back, and removes the file.
        std::vector<View> readViews(std::string_view text)
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
            const std::vector<View> read = readViews(R"(
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
            const View* start = findView(read, "start");
            ASSERT_NE(start, nullptr);
            ASSERT_TRUE(start->mRoute.has_value());

            EXPECT_EQ(start->mRoute->mTo, "finish");
            EXPECT_EQ(start->mRoute->mOrigin, osg::Vec3f(8292.0f, 200.0f, 700.0f));
            EXPECT_EQ(start->mRoute->mTarget, osg::Vec3f(8292.0f, 300.0f, 700.0f));
            EXPECT_EQ(start->mRoute->mSpeed, 1500.0f);

            // The destination is an ordinary view and goes nowhere itself.
            const View* finish = findView(read, "finish");
            ASSERT_NE(finish, nullptr);
            EXPECT_FALSE(finish->mRoute.has_value());
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

        /// A place says when it is looked at, and takes where it stands from another place.
        ///
        /// **The pair is the point.** A dawn row and a noon row of one camera mean something beside
        /// each other only where the camera is identical by construction — coordinates copied by
        /// hand drift the first time either is moved, and the pair then reads two cameras as a
        /// difference the hour made.
        TEST(RtxViewsTest, aPlaceFixesItsHourAndTakesItsCameraFromWhatItIsLike)
        {
            const std::vector<View> read = readViews(R"(
[ship]
cell = -2,-9
pos = 100, 200, 300
look = 100, 300, 300

[ship-dawn]
like = ship
hour = 6.5

[ship-dusk-from-the-mast]
like = ship
pos = 100, 200, 900
hour = 19.25
)");

            ASSERT_EQ(read.size(), std::size_t{ 3 });

            // A place that fixes nothing keeps its hour absent, which is what lets a run name one.
            const View* noon = findView(read, "ship");
            ASSERT_NE(noon, nullptr);
            EXPECT_FALSE(noon->mHour.has_value());

            const View* dawn = findView(read, "ship-dawn");
            ASSERT_NE(dawn, nullptr);
            ASSERT_TRUE(dawn->mHour.has_value());
            EXPECT_EQ(*dawn->mHour, 6.5f);

            // The whole camera, taken rather than restated.
            EXPECT_EQ(dawn->mCell, "-2,-9");
            ASSERT_TRUE(dawn->mOrigin.has_value());
            ASSERT_TRUE(dawn->mTarget.has_value());
            EXPECT_EQ(*dawn->mOrigin, osg::Vec3f(100.0f, 200.0f, 300.0f));
            EXPECT_EQ(*dawn->mTarget, osg::Vec3f(100.0f, 300.0f, 300.0f));

            // **What a borrower states itself is kept**, so a place may sit somewhere else under
            // the same cell and the same view of it.
            const View* mast = findView(read, "ship-dusk-from-the-mast");
            ASSERT_NE(mast, nullptr);
            ASSERT_TRUE(mast->mOrigin.has_value());
            EXPECT_EQ(*mast->mOrigin, osg::Vec3f(100.0f, 200.0f, 900.0f)) << "its own position was overwritten";
            EXPECT_EQ(*mast->mTarget, osg::Vec3f(100.0f, 300.0f, 300.0f)) << "the look it did not state";
            EXPECT_EQ(mast->mCell, "-2,-9");
        }

        /// Every way of writing an hour or a likeness wrong is a refusal.
        ///
        /// A view that quietly stood at another hour, or in another place, would report a number
        /// against a frame nobody asked for — which is the failure this whole file exists to stop.
        TEST(RtxViewsTest, anHourOrALikenessThatCannotBeMeantIsRefused)
        {
            constexpr std::string_view sShip = "[ship]\ncell = -2,-9\npos = 1, 2, 3\nlook = 1, 9, 3\n";

            EXPECT_THROW(readViews(std::string(sShip) + "[dawn]\nlike = ship\nhour = dawn\n"), std::runtime_error)
                << "an hour that is not a number";

            EXPECT_THROW(readViews(std::string(sShip) + "[dawn]\nlike = ship\nhour = 24\n"), std::runtime_error)
                << "an hour off the end of the clock";

            EXPECT_THROW(readViews(std::string(sShip) + "[dawn]\nlike = ship\nhour = -1\n"), std::runtime_error)
                << "an hour before the day began";

            // Midnight and a moment before the next one are both hours of the day.
            EXPECT_NO_THROW(readViews(std::string(sShip) + "[dark]\nlike = ship\nhour = 0\n"));
            EXPECT_NO_THROW(readViews(std::string(sShip) + "[late]\nlike = ship\nhour = 23.99\n"));

            EXPECT_THROW(readViews(std::string(sShip) + "[dawn]\nlike = nowhere\n"), std::runtime_error)
                << "like a view that is not there";

            EXPECT_THROW(readViews("[dawn]\nlike = dawn\n"), std::runtime_error) << "like itself";

            EXPECT_THROW(
                readViews(std::string(sShip) + "[dawn]\nlike = ship\n[later]\nlike = dawn\n"), std::runtime_error)
                << "a chain, which would make the order things are read in decide what a view is";
        }

        /// Which hour wins, which is the one rule the three commands that draw a view all read.
        TEST(RtxViewsTest, theHourOnTheCommandLineBeatsTheOneAPlaceFixes)
        {
            // Neither says anything: noon, the hour a picture of a place is taken at.
            EXPECT_EQ(hourFor(std::nullopt, std::nullopt), sDefaultHour);

            // Only the place: the place decides, which is what makes a view id one frame.
            EXPECT_EQ(hourFor(std::nullopt, 6.5f), 6.5f);

            // The command line, over a place that fixes one and over a place that does not.
            EXPECT_EQ(hourFor(9.0f, 6.5f), 9.0f);
            EXPECT_EQ(hourFor(9.0f, std::nullopt), 9.0f);

            // And the two disagree, or none of the above says anything.
            EXPECT_NE(hourFor(std::nullopt, 6.5f), hourFor(9.0f, 6.5f));
        }

        /// Where the camera stands part-way along, hand-computed and clamped at the far end.
        ///
        /// **A thousand units at a hundred a second is a tenth of the way after a second**, and the
        /// look point moves with it — the two endpoints below are each a hundred units ahead of
        /// their own position, so the camera faces the same way throughout and the frame at the
        /// halfway mark is the frame five hundred units in.
        TEST(RtxViewsTest, aRouteIsFlownAtItsSpeedAndStopsWhenItArrives)
        {
            const Placement start{ .mOrigin = { 0.0f, 0.0f, 0.0f }, .mTarget = { 0.0f, 100.0f, 0.0f } };
            const Route route{ .mTo = "finish",
                .mOrigin = { 0.0f, 1000.0f, 0.0f },
                .mTarget = { 0.0f, 1100.0f, 0.0f },
                .mSpeed = 100.0f };

            EXPECT_EQ(route.partAt(start, 0.0f), 0.0f);
            EXPECT_EQ(route.partAt(start, 1.0f), 0.1f);
            EXPECT_EQ(route.partAt(start, 5.0f), 0.5f);

            // Ten seconds is the thousand units exactly; anything past it stands at the far end.
            EXPECT_EQ(route.partAt(start, 10.0f), 1.0f);
            EXPECT_EQ(route.partAt(start, 40.0f), 1.0f);

            EXPECT_EQ(route.at(start, 0.5f).mOrigin, osg::Vec3f(0.0f, 500.0f, 0.0f));
            EXPECT_EQ(route.at(start, 0.5f).mTarget, osg::Vec3f(0.0f, 600.0f, 0.0f));
            EXPECT_EQ(route.at(start, 1.0f).mOrigin, route.mOrigin);
            EXPECT_EQ(route.at(start, 1.0f).mTarget, route.mTarget);

            // **Twice the speed is twice the distance, which is what says the speed is read at all.**
            const Route faster{
                .mTo = route.mTo, .mOrigin = route.mOrigin, .mTarget = route.mTarget, .mSpeed = 200.0f
            };
            EXPECT_EQ(faster.partAt(start, 1.0f), 0.2f);
            EXPECT_NE(faster.partAt(start, 1.0f), route.partAt(start, 1.0f));

            // A route whose ends coincide has arrived, rather than dividing by nothing.
            const Route standing{ .mTo = "here", .mOrigin = start.mOrigin, .mTarget = start.mTarget, .mSpeed = 100.0f };
            EXPECT_EQ(standing.partAt(start, 1.0f), 1.0f);
        }
    }
}
