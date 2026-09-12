#include <cstdint>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include <components/rtx/camera.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtx/scenedesc.hpp>

#include "geometry.hpp"
#include "harness.hpp"

namespace Rtx
{
    namespace
    {
        constexpr std::uint32_t sSize = 64;
        constexpr std::uint32_t sEveryPixel = sSize * sSize;

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
        class RtxFramesTest : public Testing::RendererTest
        {
        protected:
            void SetUp() override
            {
                Testing::RendererTest::SetUp();
                if (mRenderer == nullptr)
                    return;

                mRenderer->resize(sSize, sSize);

                // On a skin of one bone, so the same wall can be moved two ways: by its instance
                // and by its pose. Its bind pose is at two hundred.
                mWall = mScene.addMesh(
                    MeshArrays{ .mPositions = Testing::wallAt(200.0f), .mIndices = Testing::sQuadIndices }, {},
                    Deform::Rig, Testing::addOneBoneRig(mScene, 4));
                mInstance = mScene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(), .mMesh = mWall });
                Testing::poseByOneBone(mScene, mWall, osg::Matrixf::identity());
                mRenderer->setScene(Rtx::SceneSlot::world(), mScene, {}, SeaState{});
            }

            /// Moves the wall by its instance and hands the placement over, which goes through the
            /// instance rows and the top level.
            void moveTo(float away)
            {
                mScene.placements().move(mInstance, osg::Matrixf::translate(0.0f, away - 200.0f, 0.0f));
                mRenderer->placeScene(Rtx::SceneSlot::world(), mScene, SeaState{});
            }

            /// Moves the wall by its pose instead, which is what a skinned body does and goes
            /// through the skinning pass and the refit's positions.
            void deformTo(float away)
            {
                mScene.clearPlacement();
                Testing::poseByOneBone(mScene, mWall, osg::Matrixf::translate(0.0f, away - 200.0f, 0.0f));
                mRenderer->placeScene(Rtx::SceneSlot::world(), mScene, SeaState{});
            }

            std::uint32_t finishedHits()
            {
                const std::optional<FrameResult> result = mRenderer->finishFrame();
                EXPECT_TRUE(result.has_value()) << "a frame was in flight and none came back";
                return result.has_value() ? result->mHits : ~0u;
            }

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

        /// A cell arriving while a frame is in flight leaves that frame the world it was placed in.
        ///
        /// **What the wait in `extendScene` is for, driven rather than assumed.** An arrival appends
        /// geometry and grows every table a frame in flight may be reading, so the renderer drains
        /// the ring before it extends. This is the case that says whether it must: the suite runs
        /// under the layers' synchronization validation, so a hazard between what the arrival writes
        /// and what the frame in flight traces is reported rather than left to chance.
        ///
        /// **A hundred rigged meshes and not one, because the count is what makes a table move.**
        /// A block is only ever appended to, so one arrival proves nothing about it; a bind table,
        /// a rig's runs and the instance rows are remade by `growTo`, which doubles — so a hundred
        /// crosses several of those and each is made again while a frame still reads the one it
        /// displaced. That is the shape a cell crossing has.
        TEST_F(RtxFramesTest, aCellArrivingWhileAFrameIsInFlightLeavesThatFrameItsOwnWorld)
        {
            mRenderer->renderFrame(ahead(), FrameOptions{});

            // Behind the camera, so what the second frame sees is decided by the wall that walks
            // away rather than by a hundred quads landing over it.
            for (int at = 0; at < 100; ++at)
            {
                const Index arrived = mScene.addMesh(
                    MeshArrays{ .mPositions = Testing::wallAt(-1000.0f), .mIndices = Testing::sQuadIndices }, {},
                    Deform::Rig, Testing::addOneBoneRig(mScene, 4));
                mScene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(), .mMesh = arrived });
                Testing::poseByOneBone(mScene, arrived, osg::Matrixf::identity());
            }

            mScene.placements().move(mInstance, osg::Matrixf::translate(0.0f, -1000.0f, 0.0f));
            mRenderer->extendScene(Rtx::SceneSlot::world(), mScene, {}, SeaState{});

            mRenderer->renderFrame(ahead(), FrameOptions{});

            EXPECT_EQ(finishedHits(), sEveryPixel) << "the frame in flight lost its wall to the arrival";
            EXPECT_EQ(finishedHits(), 0u) << "the frame after the arrival kept the wall the arrival moved";
        }

        /// A third frame waits for the first, whose slot it takes, and the first still reports.
        ///
        /// **Whichever call did the waiting, the report belongs to the frame.** The ring drains
        /// itself to make room, and a caller asking once a frame is answered once a frame however
        /// many of them that drain accounted for.
        TEST_F(RtxFramesTest, aFrameTheRingDrainedToMakeRoomStillReports)
        {
            mRenderer->renderFrame(ahead(), FrameOptions{});
            moveTo(-1000.0f);
            mRenderer->renderFrame(ahead(), FrameOptions{});
            moveTo(200.0f);
            mRenderer->renderFrame(ahead(), FrameOptions{});

            // The third placement wrote the copy the first frame traced, and could only do so once
            // the first frame had finished — which is the drain, and the frame it accounted for.
            EXPECT_EQ(finishedHits(), sEveryPixel) << "the wall the first frame was drawn against";
            EXPECT_EQ(finishedHits(), 0u) << "the second, with the wall moved behind the eye";
            EXPECT_EQ(finishedHits(), sEveryPixel) << "the third, with it moved back";
            EXPECT_FALSE(mRenderer->finishFrame().has_value()) << "a frame reported twice";
        }

        /// A caller that stops collecting loses the reports that have stopped being true.
        ///
        /// **A report is a span into its frame's own timer**, good until that slot comes round and
        /// resolves again — `sFrameSlots` finishes away. Holding one past that would hand back the
        /// zones of a later frame, so the oldest goes instead. Five frames drawn and nothing asked
        /// for in between is one more finish than the ring can answer for, and the first frame is
        /// the one it cannot.
        TEST_F(RtxFramesTest, aReportIsDroppedRatherThanHeldPastTheFrameItDescribes)
        {
            for (int at = 0; at < 5; ++at)
            {
                if (at > 0)
                    moveTo(at % 2 == 0 ? 200.0f : -1000.0f);

                mRenderer->renderFrame(ahead(), FrameOptions{});
            }

            // The first frame's wall was in front of the eye; what comes back starts at the second.
            EXPECT_EQ(finishedHits(), 0u) << "the second frame, or the first held past its timer";
            EXPECT_EQ(finishedHits(), sEveryPixel);
            EXPECT_EQ(finishedHits(), 0u);
            EXPECT_EQ(finishedHits(), sEveryPixel);
            EXPECT_FALSE(mRenderer->finishFrame().has_value()) << "five frames answered five times";
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

        /// A picture inside the interface adds nothing to the frame's count, wherever between two
        /// frames it is traced.
        ///
        /// The picture is of the same wall from the same eye, so counted it would double the hits
        /// of whichever frame's buffer it landed in.
        TEST_F(RtxFramesTest, aPictureInsideTheInterfaceIsNotCountedWithTheFrame)
        {
            const GuiSlot texture = mRenderer->addGuiTexture(sSize, sSize);

            mRenderer->renderFrame(ahead(), FrameOptions{});
            mRenderer->traceGuiTexture(texture, ahead(), GuiTraceOptions{ .mWidth = sSize, .mHeight = sSize });
            mRenderer->renderFrame(ahead(), FrameOptions{});

            EXPECT_EQ(finishedHits(), sEveryPixel) << "the frame before the picture";
            EXPECT_EQ(finishedHits(), sEveryPixel) << "the frame after it";

            mRenderer->dropGuiTexture(texture);
        }

        /// A picture's copy arrives with the frame that carried it and never sooner, and a drain
        /// lands it at once.
        ///
        /// The trace rides the next submit, which is the frame after it; the copy is readable once
        /// that frame has been finished — two frames on, on the game's own cadence — and
        /// `finishGuiTraces` is the harness's way of not waiting for that.
        TEST_F(RtxFramesTest, aPicturesCopyArrivesWithTheFrameThatCarriedIt)
        {
            const GuiSlot texture = mRenderer->addGuiTexture(sSize, sSize);
            std::vector<std::uint8_t> copy(std::size_t{ sSize } * sSize * 4);

            mRenderer->traceGuiTexture(
                texture, ahead(), GuiTraceOptions{ .mWidth = sSize, .mHeight = sSize, .mReadBack = true });
            EXPECT_FALSE(mRenderer->takeGuiCopy(texture, copy)) << "recorded and carried by nothing yet";

            mRenderer->renderFrame(ahead(), FrameOptions{});
            EXPECT_FALSE(mRenderer->takeGuiCopy(texture, copy)) << "carried, and the frame is in flight";

            mRenderer->renderFrame(ahead(), FrameOptions{});
            EXPECT_EQ(finishedHits(), sEveryPixel);
            EXPECT_TRUE(mRenderer->takeGuiCopy(texture, copy)) << "the frame that carried it is finished";
            EXPECT_EQ(copy[3], 255) << "the wall, opaque, at the first pixel";

            mRenderer->traceGuiTexture(
                texture, ahead(), GuiTraceOptions{ .mWidth = sSize, .mHeight = sSize, .mReadBack = true });
            EXPECT_FALSE(mRenderer->takeGuiCopy(texture, copy)) << "a new trace is a new wait";
            mRenderer->finishGuiTraces();
            EXPECT_TRUE(mRenderer->takeGuiCopy(texture, copy)) << "drained";

            mRenderer->dropGuiTexture(texture);
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
            mRenderer->placeScene(Rtx::SceneSlot::world(), mScene, SeaState{});
            mRenderer->renderFrame(ahead(), FrameOptions{});

            // Placed into the copy the first frame is not reading, while that frame is in flight.
            moveTo(-1000.0f);
            mRenderer->renderFrame(ahead(), FrameOptions{});

            // Collected here and not at the end: the placement below writes the copy the first
            // frame read, and waits it out — and a frame nothing collected before its copy comes
            // round again is reclaimed with its numbers.
            EXPECT_EQ(finishedHits(), sEveryPixel) << "the first wall, before anything moved";

            mScene.placements().move(arrived, osg::Matrixf::identity());
            mRenderer->placeScene(Rtx::SceneSlot::world(), mScene, SeaState{});
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

            EXPECT_EQ(finishedHits(), sEveryPixel) << "the first pose, in front of the eye";
            EXPECT_EQ(finishedHits(), 0u) << "the second, moved behind it";
            EXPECT_EQ(finishedHits(), sEveryPixel) << "the third, moved back";
            EXPECT_FALSE(mRenderer->finishFrame().has_value());
        }
    }
}
