#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <apps/rtxtool/benchsuite.hpp>
#include <apps/rtxtool/views.hpp>

#include "../rtx/harness.hpp"

namespace RtxTool
{
    namespace
    {
        /// Where the resource files the tool reads are copied to.
        std::filesystem::path resources()
        {
            return Rtx::Testing::getShaderDirectory().parent_path();
        }

        TEST(RtxBenchSuiteTest, aListOfNamesLosesItsSpacingAndItsEmptyEntries)
        {
            EXPECT_EQ(splitNames("balmora,vivec"), (std::vector<std::string>{ "balmora", "vivec" }));
            EXPECT_EQ(splitNames("  balmora ,\tvivec  "), (std::vector<std::string>{ "balmora", "vivec" }));

            // A trailing comma is what a list being edited looks like halfway through, and an empty
            // entry is not a view whose name is the empty string.
            EXPECT_EQ(splitNames("balmora,,vivec,"), (std::vector<std::string>{ "balmora", "vivec" }));
            EXPECT_EQ(splitNames("one"), (std::vector<std::string>{ "one" }));
            EXPECT_TRUE(splitNames("").empty());
            EXPECT_TRUE(splitNames("  ,  ").empty());
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
            const std::vector<View> views = loadViews(resources() / "views.cfg");
            const std::vector<BenchSuite> suites = loadSuites(resources() / "benches.cfg");

            EXPECT_NE(findSuite(suites, "default"), nullptr) << "`bench` with no arguments runs [default]";

            for (const BenchSuite& suite : suites)
                for (const std::string& name : suite.mViews)
                    EXPECT_NE(findView(views, name), nullptr)
                        << "suite \"" << suite.mName << "\" names \"" << name << "\", which views.cfg has not got";
        }
    }
}
