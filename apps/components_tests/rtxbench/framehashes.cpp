#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtx/error.hpp>
#include <components/rtxbench/framehashes.hpp>

namespace Rtx
{
    namespace
    {
        constexpr std::uint8_t sPixels[] = { 1, 2, 3, 4 };
        constexpr std::uint8_t sOtherPixels[] = { 1, 2, 3, 5 };

        std::array<std::uint64_t, 2> hashOf(const std::uint64_t seed)
        {
            return { seed, seed * 7 + 1 };
        }

        /// A digest for every part, each one different, so a column that moves cannot be confused
        /// with its neighbour.
        ScenePartDigests partsOf(const std::uint64_t seed)
        {
            ScenePartDigests parts{};
            for (std::size_t at = 0; at < parts.size(); ++at)
                parts[at] = hashOf(seed + at);

            return parts;
        }

        /// One frame, noted and pictured at once.
        void add(FrameHashes& run, const std::uint32_t frame, const std::span<const std::uint8_t> pixels,
            const ScenePartDigests& parts)
        {
            run.note("somewhere", frame, frame, parts);
            run.picture(frame, pixels);
        }

        FrameHashes runOf(const ScenePartDigests& parts, const std::span<const std::uint8_t> pixels)
        {
            FrameHashes run;
            add(run, 1, pixels, parts);
            return run;
        }

        FrameHashes plainRun()
        {
            return runOf(partsOf(100), sPixels);
        }

        /// **A part that moved is the only one named, and the summary says the scene moved with
        /// it.** One number for the whole layout said a run had been handed two worlds and nothing
        /// more, so every reading of it began with a bisection by rebuild — a run apiece for one
        /// answer, on a defect that shows in half the pairs. The column is what answers it from the
        /// pair already written.
        TEST(RtxFrameHashesTest, onlyThePartThatMovedIsNamed)
        {
            ScenePartDigests moved = partsOf(100);
            moved[static_cast<std::size_t>(ScenePart::Textures)] = hashOf(4242);

            const FrameHashes was = plainRun();
            const std::vector<FrameHashes::ViewDifference> came = runOf(moved, sPixels).against(was);

            ASSERT_EQ(came.size(), 1u);
            const FrameHashes::ViewDifference& difference = came.front();

            EXPECT_TRUE(difference.mDiffering.empty()) << "the picture was the same both times";
            EXPECT_EQ(difference.mSceneDiffering, std::vector<std::uint32_t>{ 1u });
            EXPECT_FALSE(difference.same()) << "a scene that moved is a difference, picture or no picture";

            for (std::size_t at = 0; at < difference.mPartsDiffering.size(); ++at)
                EXPECT_EQ(difference.mPartsDiffering[at], at == static_cast<std::size_t>(ScenePart::Textures) ? 1u : 0u)
                    << nameOf(static_cast<ScenePart>(at));

            const std::string report = describeDifference(difference);
            EXPECT_NE(report.find("textures 1"), std::string::npos) << report;
            EXPECT_EQ(report.find("meshes"), std::string::npos) << report;
        }

        /// **A picture that moved where no part did is the renderer and not the world.** Which of
        /// the two it is decides where to look next, and one number for the whole scene could not
        /// say it: the report has to name a picture that moved on its own.
        TEST(RtxFrameHashesTest, aPictureThatMovedAloneSaysTheSceneDidNot)
        {
            const std::vector<FrameHashes::ViewDifference> came = runOf(partsOf(100), sOtherPixels).against(plainRun());

            ASSERT_EQ(came.size(), 1u);
            const FrameHashes::ViewDifference& difference = came.front();

            EXPECT_EQ(difference.mDiffering, std::vector<std::uint32_t>{ 1u });
            EXPECT_TRUE(difference.mSceneDiffering.empty()) << "no part of the scene moved";

            const std::string report = describeDifference(difference);
            EXPECT_NE(report.find("the scene was the same on every frame"), std::string::npos) << report;
        }

        /// A run written and read back is the run that was written, column for column.
        TEST(RtxFrameHashesTest, aRunSurvivesTheFileItIsWrittenTo)
        {
            const std::filesystem::path file = std::filesystem::temp_directory_path() / "openmw-rtx-hashes-test.csv";
            std::filesystem::remove(file);

            plainRun().write(file);
            const FrameHashes read = FrameHashes::read(file);

            ASSERT_EQ(read.frameCount(), 1u);

            const std::vector<FrameHashes::ViewDifference> against = plainRun().against(read);
            ASSERT_EQ(against.size(), 1u);
            EXPECT_TRUE(against.front().same());
            EXPECT_TRUE(against.front().mSceneDiffering.empty());

            // The header names every column, so a reader and an `awk` line both know what they hold.
            // Read in a scope of its own: Windows will not remove a file a stream still holds.
            std::string header;
            {
                std::ifstream in(file);
                std::getline(in, header);
            }
            EXPECT_EQ(header.substr(0, 29), "hashes 2: view,frame,picture,");
            EXPECT_NE(header.find(",textures,"), std::string::npos) << header;

            std::filesystem::remove(file);
        }

        /// **A row is two halves that meet through the frame's own number.** What a frame was
        /// handed is known as it is drawn and its picture a frame or two later, when the report
        /// comes back; a picture for a frame nobody noted — a warm-up's — is dropped, and a row
        /// nobody pictured is a file the run refuses to write rather than a hash of nothing.
        TEST(RtxFrameHashesTest, aRowIsNotedAtTheFrameAndPicturedWhenItComesBack)
        {
            FrameHashes run;
            run.note("somewhere", 1, 100, partsOf(100));
            run.note("somewhere", 2, 101, partsOf(100));
            EXPECT_EQ(run.frameCount(), 2u);
            EXPECT_EQ(run.countUnpictured(), 2u);

            run.picture(99, sPixels);
            EXPECT_EQ(run.countUnpictured(), 2u) << "a picture for a frame nobody noted made a row";

            run.picture(100, sPixels);
            run.picture(101, sOtherPixels);
            EXPECT_EQ(run.countUnpictured(), 0u);

            // The two rows read as if noted and pictured at once: the first the plain run's
            // picture, the second another.
            FrameHashes plain;
            add(plain, 1, sPixels, partsOf(100));
            add(plain, 2, sPixels, partsOf(100));
            const std::vector<FrameHashes::ViewDifference> came = run.against(plain);
            ASSERT_EQ(came.size(), 1u);
            EXPECT_EQ(came.front().mDiffering, std::vector<std::uint32_t>{ 2u });

            FrameHashes half;
            half.note("somewhere", 1, 7, partsOf(100));
            const std::filesystem::path file = std::filesystem::temp_directory_path() / "openmw-rtx-hashes-half.csv";
            EXPECT_THROW(half.write(file), Error) << "a row with no picture was written";
            std::filesystem::remove(file);
        }

        /// **A file from another build fails rather than reading as a difference.** Its columns are
        /// not this build's, so comparing them one for one would name a table nobody touched.
        TEST(RtxFrameHashesTest, aFileWhoseColumnsAreNotThisBuildsIsRefused)
        {
            const std::filesystem::path file = std::filesystem::temp_directory_path() / "openmw-rtx-hashes-old.csv";

            {
                std::ofstream out(file);
                out << "view,frame,picture,scene\n";
                out << "somewhere,1," << std::string(32, 'a') << ',' << std::string(32, 'b') << '\n';
            }

            EXPECT_THROW(FrameHashes::read(file), Error);
            std::filesystem::remove(file);
        }
    }
}
