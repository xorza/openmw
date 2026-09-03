#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Vec3f>

#include <components/rtx/renderer.hpp>

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
            request.mFrame.mFieldOfView = 60.0f;
            request.mFrame.mWeather = "Ashstorm";
            request.mFrame.mHour = 17.25f;
            request.mFrame.mFilter = false;
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

            request.mFrame.mExposure = 0.25f;
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
                " --upscale=off --preset=d --reorder=off --filter=false"
                " --validation=true --sync-validation=true --gpu-validation=false");
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
            albedo.mFrame.mShowAlbedo = true;

            const osg::Vec3f at(0.0f, 0.0f, 0.0f);
            const osg::Vec3f to(0.0f, 1.0f, 0.0f);
            const Rtx::ValidationOptions off{};
            const Rtx::ValidationOptions gpu{ .mEnabled = true, .mGpuAssisted = true };

            ViewRequest denoised = makeRequest();
            denoised.mFrame.mFilter = true;

            EXPECT_NE(describeProfile(shaded, off, at, to, 8, 8), describeProfile(albedo, off, at, to, 8, 8));
            EXPECT_NE(describeProfile(shaded, off, at, to, 8, 8), describeProfile(denoised, off, at, to, 8, 8));
            EXPECT_NE(describeProfile(shaded, off, at, to, 8, 8), describeProfile(shaded, gpu, at, to, 8, 8));
            EXPECT_TRUE(describeProfile(albedo, off, at, to, 8, 8).ends_with(" --albedo"));

            // **The two the line used to leave out.** `shot` upscales at quality unless told
            // otherwise, so a window flown with neither of these named profiled into a command that
            // rendered at another mode through another network — the same line, a different frame,
            // and a different price.
            ViewRequest upscaled = makeRequest();
            upscaled.mFrame.mUpscale = Rtx::Upscale::Performance;
            ViewRequest latest = makeRequest();
            latest.mFrame.mPreset = Rtx::Preset::E;

            EXPECT_NE(describeProfile(shaded, off, at, to, 8, 8), describeProfile(upscaled, off, at, to, 8, 8));
            EXPECT_NE(describeProfile(shaded, off, at, to, 8, 8), describeProfile(latest, off, at, to, 8, 8));

            // **A reorder is the same omission one switch later.** It costs the trace 7 to 17
            // percent and it moves a scattering of pixels, so a line that dropped it would name one
            // frame and reproduce another.
            ViewRequest sorted = makeRequest();
            sorted.mFrame.mReorder = Rtx::Reorder::Hit;
            EXPECT_NE(describeProfile(shaded, off, at, to, 8, 8), describeProfile(sorted, off, at, to, 8, 8));
        }

        /// A title whose every number is distinct, so a field printed from the wrong one shows.
        WindowTitle makeTitle()
        {
            return WindowTitle{
                .mName = "balmora",
                .mFps = 61.4,
                .mOutputWidth = 2560,
                .mOutputHeight = 1440,
                .mRenderWidth = 2560,
                .mRenderHeight = 1440,
                .mOrigin = osg::Vec3f(-19216.5f, -14896.25f, 160.0f),
                .mSpeed = 512.0f,
                .mDay = 3,
                .mHour = 17.25f,
                .mWeather = "Ashstorm",
            };
        }

        /// The bar says one size where nothing upscales and both where something does.
        ///
        /// **Two extents printed alike would be the one line that cannot say whether a run is
        /// upscaling**, which is the first thing to check when a window looks soft.
        TEST(RtxWindowTitleTest, bothExtentsAreNamedOnlyWhereTheyDiffer)
        {
            const WindowTitle same = makeTitle();
            EXPECT_NE(describeTitle(same).find("2560x1440"), std::string::npos);
            EXPECT_EQ(describeTitle(same).find(" to "), std::string::npos) << "one extent was printed as two";

            WindowTitle upscaled = same;
            upscaled.mRenderWidth = 1280;
            upscaled.mRenderHeight = 720;

            EXPECT_NE(describeTitle(upscaled).find("1280x720 to 2560x1440"), std::string::npos);
        }

        /// A settled sky names one weather and a crossing names both, with how far along it is.
        TEST(RtxWindowTitleTest, aSkyThatIsTurningSaysWhatIntoAndHowFar)
        {
            const std::string settled = describeTitle(makeTitle());
            EXPECT_NE(settled.find("Ashstorm"), std::string::npos);
            EXPECT_EQ(settled.find(" to Clear"), std::string::npos);

            WindowTitle turning = makeTitle();
            turning.mInto = "Clear";
            turning.mTurned = 0.25f;

            EXPECT_NE(describeTitle(turning).find("Ashstorm to Clear 25%"), std::string::npos);
        }

        /// Everything that moves while the window is open reaches the bar, and each of them once.
        TEST(RtxWindowTitleTest, everyMovingNumberIsInTheBar)
        {
            EXPECT_EQ(describeTitle(makeTitle()),
                "balmora  |  61 fps  |  2560x1440  |  -19216, -14896, 160  |  512 u/s  |  day 3 17:15 Ashstorm");
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

            // **Clear noon writes neither condition**, so a view pasted from an ordinary window is
            // still free to be measured under whatever a run names.
            EXPECT_FALSE(read.front().mHour.has_value()) << describeBlock(spot);
            EXPECT_FALSE(read.front().mWeather.has_value()) << describeBlock(spot);

            // **And anything else writes both**, because the light is most of what the frame is: a
            // block pasted from a window flown at dawn in a storm has to bring both with it.
            Viewpoint dawn = spot;
            dawn.mHour = 6.5f;
            dawn.mWeather = "Thunderstorm";

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
            ASSERT_TRUE(back.front().mWeather.has_value());
            EXPECT_EQ(*back.front().mWeather, "Thunderstorm");
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
    }
}
