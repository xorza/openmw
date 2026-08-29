#include <array>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include <components/rtx/camera.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtx/scenedesc.hpp>

#include "harness.hpp"

namespace Rtx
{
    namespace
    {
        constexpr std::uint32_t sSize = 64;
        constexpr std::uint32_t sEveryPixel = sSize * sSize;
        constexpr std::array<std::uint32_t, 6> sQuadIndices{ 0, 1, 2, 0, 2, 3 };

        /// A wall across the view `away` units ahead of an eye at the origin looking along +Y, and
        /// behind it where `away` is negative — so a frame either hits every pixel or none, which is
        /// what tells the frames apart.
        std::array<osg::Vec3f, 4> wallAt(float away)
        {
            return {
                osg::Vec3f(-8000.0f, away, -8000.0f),
                osg::Vec3f(8000.0f, away, -8000.0f),
                osg::Vec3f(8000.0f, away, 8000.0f),
                osg::Vec3f(-8000.0f, away, 8000.0f),
            };
        }

        Shaders::VisibilityConstants ahead()
        {
            return makeCamera(osg::Vec3f(), osg::Vec3f(0.0f, 100.0f, 0.0f), 60.0f, sSize, sSize, 100000.0f);
        }

        /// Two frames in flight, and what each of them read.
        ///
        /// **What is under test is that a frame keeps the world it was given.** The CPU places the
        /// next frame while the device draws this one, so the tables a frame traces have to be the
        /// ones it was placed with and not the ones the placement after overwrote — which is why
        /// every test here places and draws several frames before it asks about any of them.
        class RtxFramesTest : public ::testing::Test
        {
        protected:
            void SetUp() override
            {
                std::string reason;
                mRenderer = Testing::getRenderer(reason);
                if (mRenderer == nullptr)
                    GTEST_SKIP() << reason;

                mRenderer->resize(sSize, sSize);
                mWall = mScene.addMesh(wallAt(200.0f), {}, {}, sQuadIndices);
                mInstance = mScene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(), .mMesh = mWall });
                mRenderer->setScene(Rtx::sWorld, mScene, {}, SeaState{});
            }

            /// Moves the wall by its instance and hands the placement over, which goes through the
            /// instance rows and the top level.
            void moveTo(float away)
            {
                mScene.moveInstance(mInstance, osg::Matrixf::translate(0.0f, away - 200.0f, 0.0f));
                mRenderer->placeScene(Rtx::sWorld, mScene, SeaState{});
            }

            /// Moves the wall by its vertices instead, which is what a skinned body does and goes
            /// through the refit's positions.
            void deformTo(float away)
            {
                mScene.clearPlacement();
                mScene.updateMesh(mWall, wallAt(away), {});
                mRenderer->placeScene(Rtx::sWorld, mScene, SeaState{});
            }

            std::uint32_t finishedHits()
            {
                const std::optional<FrameResult> result = mRenderer->finishFrame();
                EXPECT_TRUE(result.has_value()) << "a frame was in flight and none came back";
                return result.has_value() ? result->mHits : ~0u;
            }

            Renderer* mRenderer = nullptr;
            SceneDesc mScene;
            Index mWall = 0;
            Index mInstance = 0;
        };

        /// Nothing in flight is nothing to finish, and a frame finished once is finished.
        TEST_F(RtxFramesTest, aFrameComesBackOnceAndInTheOrderItWasDrawn)
        {
            EXPECT_FALSE(mRenderer->finishFrame().has_value()) << "nothing was drawn and something came back";

            // Placed and drawn twice over before either is asked about: the second placement writes
            // the other copy of the tables, and the first frame's trace still reads its own.
            mRenderer->renderFrame(ahead(), FrameOptions{});
            moveTo(-1000.0f);
            mRenderer->renderFrame(ahead(), FrameOptions{});

            EXPECT_EQ(finishedHits(), sEveryPixel) << "the first frame read the second frame's placement";
            EXPECT_EQ(finishedHits(), 0u) << "the second frame read the first frame's placement";
            EXPECT_FALSE(mRenderer->finishFrame().has_value()) << "a frame came back twice";
        }

        /// A third frame waits for the first, whose slot it takes, and the first's numbers go with
        /// it: a caller that wants them collects them before the slot comes round.
        TEST_F(RtxFramesTest, aThirdFrameInFlightTakesTheFirstOnesSlot)
        {
            mRenderer->renderFrame(ahead(), FrameOptions{});
            moveTo(-1000.0f);
            mRenderer->renderFrame(ahead(), FrameOptions{});
            moveTo(200.0f);
            mRenderer->renderFrame(ahead(), FrameOptions{});

            // The third placement wrote the copy the first frame traced, and could only do so once
            // the first frame had finished — so what comes back starts at the second.
            EXPECT_EQ(finishedHits(), 0u) << "the second frame's count, or a frame that never finished";
            EXPECT_EQ(finishedHits(), sEveryPixel) << "the third frame read a copy the first frame was still using";
            EXPECT_FALSE(mRenderer->finishFrame().has_value());
        }

        /// Several placements before a trace are one frame, and the trace reads the last of them.
        ///
        /// **A frame the ring counts is a frame the caller asked for.** A cell crossing hands the
        /// scene over twice — once for what arrived and once for the walk behind it — and the game
        /// walks its precipitation beside its world. Each placement past the first used to close the
        /// frame and submit an empty one in its place, so a crossing spent a slot on a frame that
        /// drew nothing and handed its nought hits back as though they were the picture's.
        TEST_F(RtxFramesTest, severalPlacementsBeforeATraceAreOneFrame)
        {
            moveTo(-1000.0f);
            moveTo(200.0f);
            mRenderer->renderFrame(ahead(), FrameOptions{});

            EXPECT_EQ(finishedHits(), sEveryPixel) << "the trace read a placement other than the last";
            EXPECT_FALSE(mRenderer->finishFrame().has_value()) << "a placement came back as a frame of its own";
        }

        /// A row appended while one copy of the rows was in flight reaches the other copy whole.
        ///
        /// **The copy that was not placed when the scene grew is smaller than the mirror**, and its
        /// next placement owes it the appended row — at an offset past its end. The wall that
        /// arrives is behind the eye and the first wall is moved behind it, so a copy still holding
        /// the first wall's old row, or built from past its end, hits something.
        TEST_F(RtxFramesTest, aRowAppendedWhileTheOtherCopyWasInFlightReachesIt)
        {
            const Index arrived = mScene.addInstance(
                MeshInstance{ .mTransform = osg::Matrixf::translate(0.0f, -1200.0f, 0.0f), .mMesh = mWall });
            mRenderer->placeScene(Rtx::sWorld, mScene, SeaState{});
            mRenderer->renderFrame(ahead(), FrameOptions{});

            // Placed into the copy the first frame is not reading, while that frame is in flight.
            moveTo(-1000.0f);
            mRenderer->renderFrame(ahead(), FrameOptions{});

            // Collected here and not at the end: the placement below writes the copy the first
            // frame read, and waits it out — and a frame nothing collected before its copy comes
            // round again is reclaimed with its numbers.
            EXPECT_EQ(finishedHits(), sEveryPixel) << "the first wall, before anything moved";

            mScene.moveInstance(arrived, osg::Matrixf::identity());
            mRenderer->placeScene(Rtx::sWorld, mScene, SeaState{});
            mRenderer->renderFrame(ahead(), FrameOptions{});

            EXPECT_EQ(finishedHits(), 0u) << "both walls behind the eye, in the copy that grew late";
            EXPECT_EQ(finishedHits(), sEveryPixel) << "the wall that arrived, moved in front";
            EXPECT_FALSE(mRenderer->finishFrame().has_value());
        }

        /// A mesh whose vertices changed keeps its old ones for the frame still tracing them.
        ///
        /// The transform path above goes through the instance rows; this one goes through the
        /// refit's positions, which are the other table a placement writes and a frame reads.
        TEST_F(RtxFramesTest, aDeformedMeshKeepsItsOldVerticesForTheFrameStillTracingThem)
        {
            deformTo(400.0f);
            mRenderer->renderFrame(ahead(), FrameOptions{});
            deformTo(-1000.0f);
            mRenderer->renderFrame(ahead(), FrameOptions{});
            deformTo(400.0f);
            mRenderer->renderFrame(ahead(), FrameOptions{});

            EXPECT_EQ(finishedHits(), 0u);
            EXPECT_EQ(finishedHits(), sEveryPixel);
            EXPECT_FALSE(mRenderer->finishFrame().has_value());
        }
    }
}
