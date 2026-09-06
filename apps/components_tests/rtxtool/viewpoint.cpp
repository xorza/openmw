#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Vec3f>

#include <apps/rtxtool/viewpoint.hpp>
#include <apps/rtxtool/views.hpp>

namespace RtxTool
{
    namespace
    {
        Viewpoint makeSpot()
        {
            return Viewpoint{
                .mView = "balmora-mages-guild",
                .mNote = "a guild interior, dense with clutter",
                .mCell = "Balmora, Guild of Mages",
                .mAt = { .mEye = osg::Vec3f(-283.29843f, -671.29584f, -580.77014f),
                    .mLook = osg::Vec3f(503.60007f, -1265.436f, -747.46844f),
                    .mHour = 12.0f,
                    .mWeather = "Clear" },
            };
        }

        TEST(RtxViewpointTest, aSpotSaysWhichWayItFaces)
        {
            const auto facing = [](float x, float y, float z) {
                Viewpoint spot;
                spot.mAt.mEye = osg::Vec3f();
                spot.mAt.mLook = osg::Vec3f(x, y, z);
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
            evening.mAt.mHour = 17.25f;
            evening.mAt.mWeather = "Ashstorm";
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
            EXPECT_EQ(*read.front().mOrigin, spot.mAt.mEye);
            EXPECT_EQ(*read.front().mTarget, spot.mAt.mLook);

            // **Clear noon writes neither condition**, so a view pasted from an ordinary window is
            // still free to be measured under whatever a run names.
            EXPECT_FALSE(read.front().mHour.has_value()) << describeBlock(spot);
            EXPECT_FALSE(read.front().mWeather.has_value()) << describeBlock(spot);

            // **And anything else writes both**, because the light is most of what the frame is: a
            // block pasted from a window flown at dawn in a storm has to bring both with it.
            Viewpoint dawn = spot;
            dawn.mAt.mHour = 6.5f;
            dawn.mAt.mWeather = "Thunderstorm";

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
