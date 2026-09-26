#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Vec3f>

#include <apps/rtxtool/model/benchrun.hpp>
#include <apps/rtxtool/model/blockfile.hpp>
#include <apps/rtxtool/run.hpp>
#include <components/testing/util.hpp>

namespace RtxTool
{
    namespace
    {
        BlockFile readBlocks(const std::string& text)
        {
            std::istringstream in(text);
            return BlockFile(in, "places.cfg");
        }

        std::string refusal(const std::string& text)
        {
            try
            {
                readBlocks(text);
            }
            catch (const std::runtime_error& error)
            {
                return error.what();
            }
            return "nothing was refused";
        }

        /// The views in `text`, read the way `--list-views` reads them.
        std::vector<Stop> readViews(std::string_view text)
        {
            const std::filesystem::path file = TestingOpenMW::outputFilePath("blockfile-views.cfg");
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

        /// A point is three numbers and nothing else, spaces around each allowed; anything else is
        /// no point, the empty text among it, and the caller says so in its own words.
        TEST(RtxBlockFileTest, aPointIsThreeNumbersAndNothingElse)
        {
            EXPECT_EQ(parseVec3("1,-2.5,3"), osg::Vec3f(1.0f, -2.5f, 3.0f));
            EXPECT_EQ(parseVec3(" -8292, -73376 ,320 "), osg::Vec3f(-8292.0f, -73376.0f, 320.0f));

            for (const std::string_view text : { "", "1,2", "1,2,3,", "1,,3", "1,2,3,4", "a,b,c", "1,2,3x", ",," })
                EXPECT_FALSE(parseVec3(text).has_value()) << '"' << text << '"';

            EXPECT_EQ(trimmed(" \tcell = 0,0\r"), "cell = 0,0");
            EXPECT_EQ(trimmed("  "), "");
        }

        /// **Blocks come back in the order they were written, and a name written twice is two
        /// blocks.** The settings parser the views were read with kept a map keyed by section, so
        /// `--list-views` printed them sorted and two sections of one name merged their fields.
        TEST(RtxBlockFileTest, blocksKeepTheFilesOrderAndARepeatedNameIsTwo)
        {
            const BlockFile file = readBlocks(
                "# a comment\n\n[zulu]\ncell = 1,1\n[alpha]\nnote = first\n\n[zulu]\nnote = again\n  weather = Rain "
                "\n");

            ASSERT_EQ(file.getBlocks().size(), 3u);
            EXPECT_EQ(file.getBlocks()[0].mName, "zulu");
            EXPECT_EQ(file.getBlocks()[1].mName, "alpha");
            EXPECT_EQ(file.getBlocks()[2].mName, "zulu");
            EXPECT_EQ(file.getBlocks()[2].mLine, 8u);

            ASSERT_EQ(file.getBlocks()[0].mFields.size(), 1u) << "the second zulu's fields are its own";
            ASSERT_EQ(file.getBlocks()[2].mFields.size(), 2u);
            EXPECT_EQ(file.getBlocks()[2].mFields[1].mName, "weather");
            EXPECT_EQ(file.getBlocks()[2].mFields[1].mValue, "Rain") << "trimmed on both sides";
            EXPECT_EQ(file.getBlocks()[2].mFields[1].mLine, 10u);

            EXPECT_EQ(refusal("cell = 0,0\n"), "places.cfg:1: a field comes before the first [section]");
            EXPECT_EQ(refusal("[a]\n[b\n"), "places.cfg:2: a section's name is not closed by ]");
            EXPECT_EQ(refusal("[a]\ncell\n"),
                "places.cfg:2: \"cell\" is neither a [section], a field = value, nor a # comment");
        }

        /// The views keep the file's order, which is what `--list-views` prints, and a view file
        /// that names one view twice is refused by the second's line rather than running one of them.
        TEST(RtxBlockFileTest, theViewsKeepTheFilesOrderAndARepeatedViewIsRefused)
        {
            const std::vector<Stop> views = readViews("[zulu]\ncell = 1,1\n[alpha]\ncell = 2,2\n[mike]\ncell = 3,3\n");
            ASSERT_EQ(views.size(), 3u);
            EXPECT_EQ(views[0].mName, "zulu");
            EXPECT_EQ(views[1].mName, "alpha");
            EXPECT_EQ(views[2].mName, "mike");

            try
            {
                readViews("[zulu]\ncell = 1,1\n[alpha]\ncell = 2,2\n[zulu]\ncell = 3,3\n");
                ADD_FAILURE() << "a view named twice was read";
            }
            catch (const std::runtime_error& error)
            {
                EXPECT_TRUE(std::string_view(error.what())
                                .ends_with(":5: a second view called \"zulu\", which line 1 already defines"))
                    << error.what();
            }
        }

        /// One parser a field, whichever schema reads it: an hour, a weather, a point, a boolean and
        /// a day each say what they are not, quoted whole, at the field's own line.
        TEST(RtxBlockFileTest, theFieldParsersRefuseWhatTheyAreNot)
        {
            const BlockFile file = readBlocks(
                "[a]\nhour = 24\nweather = Drizzle\npos = 1,2\nsettled = yes\nday = -1\nspeed = 0\nhour = 6.5\n");
            const std::vector<BlockField>& fields = file.getBlocks()[0].mFields;

            const auto refused = [](const auto& read) {
                try
                {
                    read();
                }
                catch (const std::runtime_error& error)
                {
                    return std::string(error.what());
                }
                return std::string("nothing was refused");
            };

            EXPECT_EQ(refused([&] { file.hour(fields[0]); }),
                "places.cfg:2: hour \"24\" is not from 0 up to but not including 24");
            EXPECT_EQ(refused([&] { file.weather(fields[1]); }),
                "places.cfg:3: weather \"Drizzle\" is none of the weathers the content files name");
            EXPECT_EQ(refused([&] { file.point(fields[2]); }),
                "places.cfg:4: pos \"1,2\" is not three numbers separated by commas");
            EXPECT_EQ(refused([&] { file.boolean(fields[3]); }), "places.cfg:5: settled \"yes\" is not true or false");
            EXPECT_EQ(refused([&] { file.day(fields[4]); }),
                "places.cfg:6: day \"-1\" is not a whole number of days from nought");
            EXPECT_EQ(
                refused([&] { file.positive(fields[5], "a speed"); }), "places.cfg:7: speed \"0\" is not a speed");
            EXPECT_EQ(file.hour(fields[6]), 6.5f);
        }
    }
}
