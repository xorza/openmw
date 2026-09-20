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
#include <components/rtx/framedigest.hpp>
#include <components/rtx/frameimage.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtx/upscale.hpp>
#include <components/rtxbench/framehashes.hpp>
#include <components/testing/util.hpp>

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

        /// The same for what the frame traced: every channel and the composite apart, and the
        /// numbers handed to the reconstruction fixed unless a test moves one.
        FrameDigest digestOf(const std::uint64_t seed)
        {
            FrameDigest digest;
            for (std::size_t at = 0; at < digest.mImages.size(); ++at)
                digest.mImages[at] = hashOf(seed + 1000 + at);
            digest.mJitterX = 0.25f;
            digest.mJitterY = -0.125f;
            digest.mFrameDeltaMs = 16.0f;
            return digest;
        }

        /// What comes back for one frame: the picture, the digest, and what reconstructed it.
        struct Finished
        {
            std::uint64_t mFrame = 0;
            std::span<const std::uint8_t> mPixels;
            FrameDigest mDigest;
            Upscale mUpscale = Upscale::Off;

            FrameResult result() const
            {
                FrameResult finished;
                finished.mFrame = mFrame;
                finished.mPixels = mPixels;
                finished.mDigest = mDigest;
                finished.mReconstruction.mUpscaling.mMode = mUpscale;
                return finished;
            }
        };

        void add(FrameHashes& run, const std::uint32_t frame, const std::span<const std::uint8_t> pixels,
            const ScenePartDigests& parts, const FrameDigest& digest = digestOf(100),
            const Upscale upscale = Upscale::Off)
        {
            run.note("somewhere", frame, frame, parts);
            run.picture(Finished{ frame, pixels, digest, upscale }.result());
        }

        FrameHashes runOf(const ScenePartDigests& parts, const std::span<const std::uint8_t> pixels,
            const FrameDigest& digest = digestOf(100), const Upscale upscale = Upscale::Off)
        {
            FrameHashes run;
            add(run, 1, pixels, parts, digest, upscale);
            return run;
        }

        FrameHashes plainRun(const Upscale upscale = Upscale::Off)
        {
            return runOf(partsOf(100), sPixels, digestOf(100), upscale);
        }

        FrameHashes::ViewDifference onlyView(const std::vector<FrameHashes::ViewDifference>& came)
        {
            EXPECT_EQ(came.size(), 1u);
            return came.empty() ? FrameHashes::ViewDifference{} : came.front();
        }

        TEST(RtxFrameHashesTest, onlyThePartThatMovedIsNamed)
        {
            ScenePartDigests moved = partsOf(100);
            moved[static_cast<std::size_t>(ScenePart::Textures)] = hashOf(4242);

            const FrameHashes::ViewDifference difference = onlyView(runOf(moved, sPixels).against(plainRun()));

            EXPECT_TRUE(difference.mDiffering.empty()) << "the picture was the same both times";
            EXPECT_TRUE(difference.mTraceDiffering.empty()) << "the trace was the same both times";
            EXPECT_EQ(difference.mSceneDiffering, std::vector<std::uint32_t>{ 1u });
            EXPECT_FALSE(difference.same()) << "a scene that moved is a difference, picture or no picture";

            for (std::size_t at = 0; at < difference.mPartsDiffering.size(); ++at)
                EXPECT_EQ(difference.mPartsDiffering[at], at == static_cast<std::size_t>(ScenePart::Textures) ? 1u : 0u)
                    << nameOf(static_cast<ScenePart>(at));

            const std::string report = describeDifference(difference);
            EXPECT_NE(report.find("textures 1"), std::string::npos) << report;
            EXPECT_EQ(report.find("meshes"), std::string::npos) << report;
        }

        TEST(RtxFrameHashesTest, aPictureThatMovedAloneSaysTheSceneDidNot)
        {
            const FrameHashes::ViewDifference difference
                = onlyView(runOf(partsOf(100), sOtherPixels).against(plainRun()));

            EXPECT_EQ(difference.mDiffering, std::vector<std::uint32_t>{ 1u });
            EXPECT_TRUE(difference.mReconstructedDiffering.empty()) << "nothing upscaled it, so the picture is ours";
            EXPECT_TRUE(difference.mTraceDiffering.empty()) << "the trace was the same, so it is the display chain";
            EXPECT_TRUE(difference.mSceneDiffering.empty()) << "no part of the scene moved";
            EXPECT_FALSE(difference.same());

            const std::string report = describeDifference(difference);
            EXPECT_NE(report.find("the picture differs on 1 of 1 frames, at 1"), std::string::npos) << report;
            EXPECT_NE(report.find("the scene was the same on every frame"), std::string::npos) << report;
        }

        TEST(RtxFrameHashesTest, aPictureThatMovedPastANetworkIsReportedAndNeverAVerdict)
        {
            const FrameHashes::ViewDifference difference = onlyView(
                runOf(partsOf(100), sOtherPixels, digestOf(100), Upscale::Quality).against(plainRun(Upscale::Quality)));

            EXPECT_TRUE(difference.mDiffering.empty()) << "the picture is the network's";
            EXPECT_EQ(difference.mReconstructedDiffering, std::vector<std::uint32_t>{ 1u });
            EXPECT_TRUE(difference.same()) << "the trace and the scene were the same, which is the whole verdict";

            const std::string report = describeDifference(difference);
            EXPECT_NE(report.find("the trace and the scene the same on every one"), std::string::npos) << report;
            EXPECT_NE(report.find("the reconstructed picture differs on 1"), std::string::npos) << report;
            EXPECT_NE(report.find("not a verdict"), std::string::npos) << report;

            // And either run past a network is enough: a reference drawn without one and a run
            // drawn with one are two configurations, which is its own finding.
            const FrameHashes::ViewDifference mixed
                = onlyView(runOf(partsOf(100), sOtherPixels, digestOf(100), Upscale::Quality).against(plainRun()));
            EXPECT_TRUE(mixed.mDiffering.empty());
            EXPECT_EQ(mixed.mReconstructedDiffering, std::vector<std::uint32_t>{ 1u });
            EXPECT_EQ(mixed.mUpscaledDiffering, 1u);
            EXPECT_FALSE(mixed.same());
            EXPECT_NE(describeDifference(mixed).find("reconstructed 1 frames differently"), std::string::npos)
                << describeDifference(mixed);
        }

        TEST(RtxFrameHashesTest, aTraceThatMovedIsTheVerdictWhateverThePictureDid)
        {
            FrameDigest moved = digestOf(100);
            moved.mImages[bindingOf(Channel::Albedo)] = hashOf(4242);

            // The same picture, past a network: the trace column alone says the run moved.
            const FrameHashes::ViewDifference difference
                = onlyView(runOf(partsOf(100), sPixels, moved, Upscale::Quality).against(plainRun(Upscale::Quality)));

            EXPECT_EQ(difference.mTraceDiffering, std::vector<std::uint32_t>{ 1u });
            EXPECT_TRUE(difference.mDiffering.empty());
            EXPECT_TRUE(difference.mReconstructedDiffering.empty());
            EXPECT_FALSE(difference.same());

            for (std::size_t column = 0; column < sTracedColumns; ++column)
                EXPECT_EQ(difference.mTracedDiffering[column], column == bindingOf(Channel::Albedo) ? 1u : 0u)
                    << tracedName(column);

            const std::string report = describeDifference(difference);
            EXPECT_NE(report.find("the trace differs on 1 of 1 frames, at 1 — g-albedo 1"), std::string::npos)
                << report;
            EXPECT_EQ(report.find("g-guide"), std::string::npos) << report;
            EXPECT_NE(report.find("the scene was the same on every frame"), std::string::npos) << report;
        }

        TEST(RtxFrameHashesTest, whatTheFrameHandedTheReconstructionIsAColumnOfItsOwn)
        {
            FrameDigest jittered = digestOf(100);
            jittered.mJitterX = -jittered.mJitterX;

            const FrameHashes::ViewDifference difference
                = onlyView(runOf(partsOf(100), sPixels, jittered).against(plainRun()));

            EXPECT_EQ(difference.mTraceDiffering, std::vector<std::uint32_t>{ 1u });
            EXPECT_EQ(difference.mTracedDiffering[sReconstructionColumn], 1u);
            EXPECT_EQ(difference.mTracedDiffering[Shaders::DIGEST_COMPOSITE], 0u) << "the images were the same";
            EXPECT_NE(describeDifference(difference).find("reconstruction 1"), std::string::npos)
                << describeDifference(difference);

            FrameDigest reset = digestOf(100);
            reset.mReset = 1;
            EXPECT_EQ(onlyView(runOf(partsOf(100), sPixels, reset).against(plainRun())).mTraceDiffering,
                std::vector<std::uint32_t>{ 1u })
                << "a reset the reference did not send is a difference";
        }

        TEST(RtxFrameHashesTest, aRunSurvivesTheFileItIsWrittenTo)
        {
            const std::filesystem::path file = TestingOpenMW::outputFilePath("hashes-test.csv");
            std::filesystem::remove(file);

            plainRun(Upscale::Quality).write(file);
            const FrameHashes read = FrameHashes::read(file);

            ASSERT_EQ(read.frameCount(), 1u);

            const FrameHashes::ViewDifference against = onlyView(plainRun(Upscale::Quality).against(read));
            EXPECT_TRUE(against.same());
            EXPECT_TRUE(against.mSceneDiffering.empty());
            EXPECT_EQ(against.mUpscaledDiffering, 0u) << "what reconstructed the picture survived the file";

            // Every column comes back: a run against the file that moved one is told so.
            FrameDigest moved = digestOf(100);
            moved.mImages[Shaders::DIGEST_COMPOSITE] = hashOf(4242);
            EXPECT_EQ(onlyView(runOf(partsOf(100), sPixels, moved, Upscale::Quality).against(read))
                          .mTracedDiffering[Shaders::DIGEST_COMPOSITE],
                1u);
            EXPECT_EQ(onlyView(plainRun().against(read)).mUpscaledDiffering, 1u);

            std::string header;
            {
                std::ifstream in(file);
                std::getline(in, header);
            }
            EXPECT_EQ(header.substr(0, 37), "hashes 4: view,frame,upscale,picture,");
            EXPECT_NE(header.find(",g-direct,"), std::string::npos) << header;
            EXPECT_NE(header.find(",composite,reconstruction,positions,"), std::string::npos) << header;
            EXPECT_NE(header.find(",textures,"), std::string::npos) << header;

            std::filesystem::remove(file);
        }

        TEST(RtxFrameHashesTest, aRowIsNotedAtTheFrameAndPicturedWhenItComesBack)
        {
            FrameHashes run;
            run.note("somewhere", 1, 100, partsOf(100));
            run.note("somewhere", 2, 101, partsOf(100));
            EXPECT_EQ(run.frameCount(), 2u);
            EXPECT_EQ(run.countUnpictured(), 2u);

            run.picture(Finished{ 99, sPixels, digestOf(100) }.result());
            EXPECT_EQ(run.countUnpictured(), 2u) << "a picture for a frame nobody noted made a row";

            run.picture(Finished{ 100, sPixels, digestOf(100) }.result());
            run.picture(Finished{ 101, sOtherPixels, digestOf(100) }.result());
            EXPECT_EQ(run.countUnpictured(), 0u);

            FrameHashes plain;
            add(plain, 1, sPixels, partsOf(100));
            add(plain, 2, sPixels, partsOf(100));
            EXPECT_EQ(onlyView(run.against(plain)).mDiffering, std::vector<std::uint32_t>{ 2u });

            // **The reference's views in another order, and one frame more on each side.** A frame
            // is found by its view and its number wherever that view stands in the file, a frame
            // the reference lacks is unmatched, and a frame the run lacks is unmatched to the
            // view's count too. The differing frame is still frame 2 of `somewhere`.
            FrameHashes elsewhereFirst;
            elsewhereFirst.note("elsewhere", 1, 50, partsOf(100));
            elsewhereFirst.picture(Finished{ 50, sPixels, digestOf(100) }.result());
            add(elsewhereFirst, 1, sPixels, partsOf(100));
            add(elsewhereFirst, 2, sPixels, partsOf(100));
            add(elsewhereFirst, 3, sPixels, partsOf(100));
            run.note("somewhere", 4, 102, partsOf(100));
            run.picture(Finished{ 102, sPixels, digestOf(100) }.result());

            const std::vector<FrameHashes::ViewDifference> reordered = run.against(elsewhereFirst);
            ASSERT_EQ(reordered.size(), 1u);
            EXPECT_EQ(reordered.front().mView, "somewhere");
            EXPECT_EQ(reordered.front().mFrames, 3u);
            EXPECT_EQ(reordered.front().mDiffering, std::vector<std::uint32_t>{ 2u });
            EXPECT_EQ(reordered.front().mUnmatched, 1u) << "frame 4 the reference lacks; frame 3 the run lacks "
                                                           "is covered by the count of 3 against 3";

            FrameHashes half;
            half.note("somewhere", 1, 7, partsOf(100));
            const std::filesystem::path file = TestingOpenMW::outputFilePath("hashes-half.csv");
            EXPECT_THROW(half.write(file), Error) << "a row with no picture was written";
            std::filesystem::remove(file);
        }

        TEST(RtxFrameHashesTest, aFileWhoseViewIsOutOfFrameOrderIsRefused)
        {
            FrameHashes run;
            add(run, 1, sPixels, partsOf(100));
            add(run, 2, sPixels, partsOf(100));
            const std::filesystem::path file = TestingOpenMW::outputFilePath("hashes-order.csv");
            run.write(file);

            std::ifstream in(file);
            std::string header;
            std::string first;
            std::string second;
            std::getline(in, header);
            std::getline(in, first);
            std::getline(in, second);
            in.close();

            std::ofstream out(file);
            out << header << '\n' << second << '\n' << first << '\n';
            out.close();

            EXPECT_THROW(FrameHashes::read(file), Error) << "frame 2 before frame 1 of one view";
            std::filesystem::remove(file);
        }

        TEST(RtxFrameHashesTest, aFileWhoseColumnsAreNotThisBuildsIsRefused)
        {
            const std::filesystem::path file = TestingOpenMW::outputFilePath("hashes-old.csv");

            {
                std::ofstream out(file);
                out << "hashes 3: view,frame,picture,positions\n";
                out << "somewhere,1," << std::string(32, 'a') << ',' << std::string(32, 'b') << '\n';
            }

            EXPECT_THROW(FrameHashes::read(file), Error);
            std::filesystem::remove(file);
        }
    }
}
