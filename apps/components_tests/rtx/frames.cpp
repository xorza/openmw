#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Matrixf>
#include <osg/Vec2f>
#include <osg/Vec3f>

#include <components/rtx/camera.hpp>
#include <components/rtx/mesh.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtx/runs.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/shaders/visibility.h>
#include <components/rtx/slot.hpp>
#include <components/rtxvulkan/device.hpp>
#include <components/rtxvulkan/sceneacceleration.hpp>
#include <components/rtxvulkan/timeline.hpp>

#include "geometry.hpp"
#include "harness.hpp"
#include "testcamera.hpp"

namespace Rtx
{
    namespace
    {
        constexpr std::uint32_t sSize = 64;
        constexpr std::uint32_t sEveryPixel = sSize * sSize;

        Shaders::VisibilityConstants ahead()
        {
            return Testing::makeCamera(osg::Vec3f(), osg::Vec3f(0.0f, 100.0f, 0.0f), 60.0f, sSize, sSize, 100000.0f);
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
                mWall = Testing::addOneBoneBody(
                    mScene, MeshArrays{ .mPositions = Testing::wallAt(200.0f), .mIndices = Testing::sQuadIndices })
                            .mMesh;
                mInstance = mScene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(), .mMesh = mWall });
                Testing::poseByOneBone(mScene, mWall, osg::Matrixf::identity());
                mRenderer->setScene(Rtx::SceneSlot::world(), mScene, {});
            }

            /// Moves the wall by its instance and hands the placement over, which goes through the
            /// instance rows and the top level.
            void moveTo(float away)
            {
                mScene.placements().move(mInstance, osg::Matrixf::translate(0.0f, away - 200.0f, 0.0f));
                mRenderer->placeScene(Rtx::SceneSlot::world(), mScene);
            }

            /// Moves the wall by its pose instead, which is what a skinned body does and goes
            /// through the skinning pass and the refit's positions.
            void deformTo(float away)
            {
                mScene.clearPlacement();
                Testing::poseByOneBone(mScene, mWall, osg::Matrixf::translate(0.0f, away - 200.0f, 0.0f));
                mRenderer->placeScene(Rtx::SceneSlot::world(), mScene);
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
                const Index arrived = Testing::addOneBoneBody(
                    mScene, MeshArrays{ .mPositions = Testing::wallAt(-1000.0f), .mIndices = Testing::sQuadIndices })
                                          .mMesh;
                mScene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(), .mMesh = arrived });
                Testing::poseByOneBone(mScene, arrived, osg::Matrixf::identity());
            }

            mScene.placements().move(mInstance, osg::Matrixf::translate(0.0f, -1000.0f, 0.0f));
            mRenderer->extendScene(Rtx::SceneSlot::world(), mScene, {});

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
            // Placed before every frame, the first included, so the three record the same zones.
            moveTo(200.0f);
            mRenderer->renderFrame(ahead(), FrameOptions{});
            moveTo(-1000.0f);
            mRenderer->renderFrame(ahead(), FrameOptions{});
            moveTo(200.0f);
            mRenderer->renderFrame(ahead(), FrameOptions{});

            // The third placement wrote the copy the first frame traced, and could only do so once
            // the first frame had finished — which is the drain, and the frame it accounted for.
            const std::optional<FrameResult> first = mRenderer->finishFrame();
            const std::optional<FrameResult> second = mRenderer->finishFrame();
            const std::optional<FrameResult> third = mRenderer->finishFrame();
            ASSERT_TRUE(first.has_value() && second.has_value() && third.has_value())
                << "three frames were in flight and fewer came back";
            EXPECT_EQ(first->mHits, sEveryPixel) << "the wall the first frame was drawn against";
            EXPECT_EQ(second->mHits, 0u) << "the second, with the wall moved behind the eye";
            EXPECT_EQ(third->mHits, sEveryPixel) << "the third, with it moved back";
            EXPECT_FALSE(mRenderer->finishFrame().has_value()) << "a frame reported twice";

            // The zones as well as the count: the third frame took the first one's slot and began
            // its timer before the first report was read, and the report is what its frame measured
            // and not what the timer holds now.
            EXPECT_EQ(first->mGpu.spans().size(), third->mGpu.spans().size())
                << "the drained frame's zones went with its slot";
        }

        /// What the game calls before each placement: it waits only where the ring is full, so the
        /// frame behind stays on the device while the next is placed, and reports the frame before.
        TEST_F(RtxFramesTest, collectFrameWaitsOnlyWhereTheRingIsFull)
        {
            EXPECT_FALSE(mRenderer->collectFrame().has_value()) << "nothing was drawn and something came back";

            mRenderer->renderFrame(ahead(), FrameOptions{});
            EXPECT_FALSE(mRenderer->collectFrame().has_value())
                << "one frame in flight is room for another, and it was waited out";

            moveTo(-1000.0f);
            mRenderer->renderFrame(ahead(), FrameOptions{});

            // Two in flight is none to spare: the first is waited out and reported, the second stays.
            const std::optional<FrameResult> first = mRenderer->collectFrame();
            ASSERT_TRUE(first.has_value()) << "the ring was full and nothing was finished";
            EXPECT_EQ(first->mHits, sEveryPixel) << "the first frame, or the second reported first";
            EXPECT_FALSE(mRenderer->collectFrame().has_value()) << "the frame behind was waited out with room to spare";

            EXPECT_EQ(finishedHits(), 0u) << "the second frame, or the first reported twice";
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
            mRenderer->traceGuiTexture(texture, ahead(), GuiTraceOptions{});
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

            mRenderer->traceGuiTexture(texture, ahead(), GuiTraceOptions{ .mReadBack = true });
            EXPECT_FALSE(mRenderer->takeGuiCopy(texture, copy)) << "recorded and carried by nothing yet";

            mRenderer->renderFrame(ahead(), FrameOptions{});
            EXPECT_FALSE(mRenderer->takeGuiCopy(texture, copy)) << "carried, and the frame is in flight";

            mRenderer->renderFrame(ahead(), FrameOptions{});
            EXPECT_EQ(finishedHits(), sEveryPixel);
            EXPECT_TRUE(mRenderer->takeGuiCopy(texture, copy)) << "the frame that carried it is finished";
            EXPECT_EQ(copy[3], 255) << "the wall, opaque, at the first pixel";

            mRenderer->traceGuiTexture(texture, ahead(), GuiTraceOptions{ .mReadBack = true });
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
            mRenderer->placeScene(Rtx::SceneSlot::world(), mScene);
            mRenderer->renderFrame(ahead(), FrameOptions{});

            // Placed into the copy the first frame is not reading, while that frame is in flight.
            moveTo(-1000.0f);
            mRenderer->renderFrame(ahead(), FrameOptions{});

            // Collected here and not at the end: the placement below writes the copy the first
            // frame read, and waits it out — and a frame nothing collected before its copy comes
            // round again is reclaimed with its numbers.
            EXPECT_EQ(finishedHits(), sEveryPixel) << "the first wall, before anything moved";

            mScene.placements().move(arrived, osg::Matrixf::identity());
            mRenderer->placeScene(Rtx::SceneSlot::world(), mScene);
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

        /// A surface moved by its pose reprojects exactly as the same surface moved by its instance:
        /// the motion vector knows where the triangle stood, and not only where the body did.
        ///
        /// **The other copy of the poses is last frame's, and this is what reads it.** A frame
        /// traces the copy its placement synced; the copy it did not trace was synced the frame
        /// before, so it holds every pose as of then — `GpuTables::mPreviousPoseBlocks` — and a
        /// hit on a body takes its own step off the pair. Four units along +x at two hundred units
        /// off is the 1.1085 pixels `aMotionVectorSaysWhereItsSurfaceWasAndNotWhereTheWorldIs`
        /// derives for a stepping camera, with the sign the surface's own: it went right, so the
        /// point now under the centre pixel was to its left. Read twice, so the sign is not a
        /// guess about the instance path, and once more on a frame nothing moved, which is the
        /// copies agreeing.
        TEST_F(RtxFramesTest, aPoseMovesAMotionVectorAsAnInstanceDoes)
        {
            constexpr std::size_t centre = std::size_t{ sSize / 2 } * sSize + sSize / 2;
            const auto centreMotion = [&] {
                std::vector<float> motion;
                mRenderer->readChannel(Channel::Motion, motion);
                return osg::Vec2f(motion[centre * 2], motion[centre * 2 + 1]);
            };

            // By the instance, as the reprojection always knew how to.
            mRenderer->renderFrame(ahead(), FrameOptions{});
            mScene.placements().move(mInstance, osg::Matrixf::translate(4.0f, 0.0f, 0.0f));
            mRenderer->placeScene(Rtx::SceneSlot::world(), mScene);
            mRenderer->renderFrame(ahead(), FrameOptions{});
            const osg::Vec2f byInstance = centreMotion();
            EXPECT_NEAR(byInstance.x(), -1.1085f, 0.02f) << "the surface went right, so the point came from the left";
            EXPECT_NEAR(byInstance.y(), 0.0f, 1e-3f);

            // Back where it was, and then by the pose alone: the instance stands still and the
            // bone carries the wall the same four units.
            mScene.placements().move(mInstance, osg::Matrixf::identity());
            mRenderer->placeScene(Rtx::SceneSlot::world(), mScene);
            mRenderer->renderFrame(ahead(), FrameOptions{});
            mScene.clearPlacement();
            Testing::poseByOneBone(mScene, mWall, osg::Matrixf::translate(4.0f, 0.0f, 0.0f));
            mRenderer->placeScene(Rtx::SceneSlot::world(), mScene);
            mRenderer->renderFrame(ahead(), FrameOptions{});
            const osg::Vec2f byPose = centreMotion();
            EXPECT_NEAR(byPose.x(), byInstance.x(), 1e-3f) << "a pose and an instance moved the same four units";
            EXPECT_NEAR(byPose.y(), byInstance.y(), 1e-3f);

            // A frame on which the body did not move: the copy this frame traces and the copy it
            // did not hold the same pose, so the step is nought exactly and not a rounding.
            mScene.clearPlacement();
            mRenderer->placeScene(Rtx::SceneSlot::world(), mScene);
            mRenderer->renderFrame(ahead(), FrameOptions{});
            const osg::Vec2f still = centreMotion();
            EXPECT_EQ(still.x(), 0.0f) << "nothing moved and the vector says something did";
            EXPECT_EQ(still.y(), 0.0f);

            while (mRenderer->finishFrame().has_value())
            {
            }
        }

        /// A refitted structure is built whole again on a rota: the posed body built longest ago,
        /// once `sRebuildEvery` posed placements have passed, one a placement.
        ///
        /// **Counted through the scene's report and driven through the placements a frame makes**,
        /// so the rule is read where a run reads it. The wall arrived on placement nought; the
        /// first sixty-three posed placements refit it and the sixty-fourth builds it whole. A
        /// second body then shares the rota with it and the two alternate, sixty-four apart each,
        /// so no placement builds two. A body posed once and left standing is not in a placement's
        /// `deformed` and is never picked, however long it stands.
        TEST_F(RtxFramesTest, aRefittedStructureIsBuiltWholeAgainOnARota)
        {
            // The fixture's `setScene` left the wall among the arrivals, as the uploader would not
            // have: an extension below must bring the second body alone, or it builds the wall
            // again and notes it built.
            mScene.clearArrivals();

            const auto rebuilt = [&] { return mRenderer->getSceneStats().mRebuilt; };

            for (std::uint64_t placement = 1; placement < SceneAcceleration::sRebuildEvery; ++placement)
            {
                deformTo(200.0f + static_cast<float>(placement % 4));
                ASSERT_EQ(rebuilt(), 0u) << "built whole on placement " << placement << ", short of the rota";
            }
            deformTo(201.0f);
            EXPECT_EQ(rebuilt(), 1u) << "the sixty-fourth posed placement builds it whole";
            deformTo(202.0f);
            EXPECT_EQ(rebuilt(), 1u) << "and the one after refits again";

            // Frames, so the placements above are drawn and the rebuilt structure is traced: a
            // structure built whole in place of a refit is the same wall to a ray.
            mRenderer->renderFrame(ahead(), FrameOptions{});
            EXPECT_EQ(finishedHits(), sEveryPixel);

            // A second body, arriving now: its arrival builds it whole and the rota counts from
            // there, so the placement that brings it builds nothing whole again, and neither do the
            // sixty-two after — the wall's turn comes first, sixty-four placements after its own.
            const Index second = Testing::addOneBoneBody(
                mScene, MeshArrays{ .mPositions = Testing::wallAt(300.0f), .mIndices = Testing::sQuadIndices })
                                     .mMesh;
            mScene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(), .mMesh = second });
            Testing::poseByOneBone(mScene, second, osg::Matrixf::identity());
            mRenderer->extendScene(Rtx::SceneSlot::world(), mScene, {});
            mScene.clearArrivals();
            EXPECT_EQ(rebuilt(), 1u) << "an arrival is not built twice on the placement that brings it";

            const auto deformBoth = [&](float x) {
                mScene.clearPlacement();
                Testing::poseByOneBone(mScene, mWall, osg::Matrixf::translate(x, 0.0f, 0.0f));
                Testing::poseByOneBone(mScene, second, osg::Matrixf::translate(-x, 0.0f, 0.0f));
                mRenderer->placeScene(Rtx::SceneSlot::world(), mScene);
            };
            // The wall was built whole on the sixty-fourth posed placement and the arrival on the
            // sixty-fifth, and the extension's own placement was the sixty-sixth. Sixty-one more
            // bring the clock to one short of the wall's turn.
            for (std::uint64_t placement = 0; placement < SceneAcceleration::sRebuildEvery - 3; ++placement)
                deformBoth(static_cast<float>(placement % 3));
            EXPECT_EQ(rebuilt(), 1u) << "neither is due yet";
            deformBoth(1.0f);
            EXPECT_EQ(rebuilt(), 2u) << "the wall's turn, sixty-four placements after its last";
            deformBoth(2.0f);
            EXPECT_EQ(rebuilt(), 3u) << "and the second body's the placement after: one a placement";
            deformBoth(0.0f);
            EXPECT_EQ(rebuilt(), 3u) << "and then neither, for another sixty-three";

            // And a placement that poses neither is not a placement of the rota at all, however
            // long it has been.
            mScene.clearPlacement();
            mRenderer->placeScene(Rtx::SceneSlot::world(), mScene);
            EXPECT_EQ(rebuilt(), 3u);

            // A frame over the placements above, because the zones a placement opens are the next
            // frame's report: drawn here they are this test's, and the shared renderer's next
            // frame is its own again.
            mRenderer->renderFrame(ahead(), FrameOptions{});
            EXPECT_EQ(finishedHits(), sEveryPixel);
        }

        /// A picture still deferred traces the copy it was placed with, however many placements of
        /// its scene follow it in the frame.
        ///
        /// **Content, not memory.** A picture inside the interface is a deferred batch: its
        /// placement built the top level from the rows of the moment and its trace reads that
        /// copy's instance table, both carried by the next submit. A third placement of the scene
        /// in the same frame writes that copy again from the host, ahead of the submit — no race,
        /// because nothing is on the queue yet, and so nothing the tables' stamps would wait for.
        /// The picture would then trace the first placement's top level against the third's rows,
        /// and here read the wall's opacity as the fade the third placement wrote and see through
        /// it. So a placement into a copy whose picture is still deferred carries the picture
        /// first, and a placement into the other copy does not.
        TEST_F(RtxFramesTest, aPlacementIntoTheCopyADeferredPictureReadsCarriesThePictureFirst)
        {
            // A scene of its own, because a picture's placements are deferred like its trace: the
            // world's placement would carry the picture on its own submit.
            const SceneSlot doll = mRenderer->addViewScene();
            SceneDesc scene;
            const Index wall
                = scene.addMesh(MeshArrays{ .mPositions = Testing::wallAt(200.0f), .mIndices = Testing::sQuadIndices });
            const Index standing
                = scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(), .mMesh = wall });
            mRenderer->setScene(doll, scene, {});

            const GuiSlot texture = mRenderer->addGuiTexture(sSize, sSize);
            std::vector<std::uint8_t> copy(std::size_t{ sSize } * sSize * 4);

            // The picture, of the copy the load wrote, with the wall whole. Over nothing, so a
            // pixel the wall does not cover is the one number that says so.
            Shaders::VisibilityConstants camera = ahead();
            camera.mTransparentBackground = 1;
            mRenderer->traceGuiTexture(texture, camera, GuiTraceOptions{ .mScene = doll, .mReadBack = true });

            // Faded out, and placed twice: into the other copy, which leaves the picture deferred,
            // and then into the picture's own.
            scene.placements().fade(standing, 0.0f);
            mRenderer->placeScene(doll, scene);
            EXPECT_FALSE(mRenderer->takeGuiCopy(texture, copy)) << "a placement into the other copy carried it";
            mRenderer->placeScene(doll, scene);

            mRenderer->finishGuiTraces();
            ASSERT_TRUE(mRenderer->takeGuiCopy(texture, copy));
            EXPECT_EQ(copy[3], 255) << "the picture saw through the fade a later placement wrote into its copy";

            mRenderer->dropGuiTexture(texture);
            mRenderer->dropViewScene(doll);
        }

        /// A picture's scene is built on a batch that rides the next submit, as an arrival is:
        /// opening one submits nothing and so waits for nothing, and opening another in its slot
        /// buries the first, so a picture of it recorded and not yet carried still comes back
        /// whole. A drain here idled the device every time the inventory or the race menu opened.
        TEST_F(RtxFramesTest, aPictureSceneOpensWithoutASubmitAndKeepsAPictureOfTheOneItReplaced)
        {
            const SceneSlot doll = mRenderer->addViewScene();
            SceneDesc scene;
            const Index wall
                = scene.addMesh(MeshArrays{ .mPositions = Testing::wallAt(200.0f), .mIndices = Testing::sQuadIndices });
            scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(), .mMesh = wall });

            const Timeline& timeline = mRenderer->getDevice().getTimeline();
            const std::uint64_t opened = timeline.getNext();
            mRenderer->setScene(doll, scene, {});
            EXPECT_EQ(timeline.getNext(), opened) << "opening a picture's scene submitted";

            const GuiSlot texture = mRenderer->addGuiTexture(sSize, sSize);
            std::vector<std::uint8_t> copy(std::size_t{ sSize } * sSize * 4);

            // Over nothing, so a pixel the wall does not cover is the one number that says so.
            Shaders::VisibilityConstants camera = ahead();
            camera.mTransparentBackground = 1;
            mRenderer->traceGuiTexture(texture, camera, GuiTraceOptions{ .mScene = doll, .mReadBack = true });

            // An empty scene in its place, with the picture of the wall still deferred.
            const std::uint64_t replaced = timeline.getNext();
            mRenderer->setScene(doll, SceneDesc{}, {});
            EXPECT_EQ(timeline.getNext(), replaced) << "replacing a picture's scene submitted";
            EXPECT_FALSE(mRenderer->takeGuiCopy(texture, copy)) << "replacing the scene carried the picture";

            mRenderer->finishGuiTraces();
            ASSERT_TRUE(mRenderer->takeGuiCopy(texture, copy));
            EXPECT_EQ(copy[3], 255) << "the picture was traced against the scene that replaced it";

            // And a picture taken now is of the empty scene.
            mRenderer->traceGuiTexture(texture, camera, GuiTraceOptions{ .mScene = doll, .mReadBack = true });
            mRenderer->finishGuiTraces();
            ASSERT_TRUE(mRenderer->takeGuiCopy(texture, copy));
            EXPECT_EQ(copy[3], 0) << "the replacement was not what the next picture traced";

            mRenderer->dropGuiTexture(texture);
            mRenderer->dropViewScene(doll);
        }

        /// A picture's scene given back is not drained: the picture recorded against it and not
        /// yet carried still rides the next submit and still comes back whole, the frames drawn
        /// after the drop are placed and traced without a wait, and the layers see nothing read
        /// after it went. A drain here idled the device every time the inventory closed.
        TEST_F(RtxFramesTest, aViewSceneGivenBackWithAPictureStillDeferredIsCarriedAndThenLetGo)
        {
            const SceneSlot doll = mRenderer->addViewScene();
            SceneDesc scene;
            const Index wall
                = scene.addMesh(MeshArrays{ .mPositions = Testing::wallAt(200.0f), .mIndices = Testing::sQuadIndices });
            scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(), .mMesh = wall });
            mRenderer->setScene(doll, scene, {});

            const GuiSlot texture = mRenderer->addGuiTexture(sSize, sSize);
            std::vector<std::uint8_t> copy(std::size_t{ sSize } * sSize * 4);

            Shaders::VisibilityConstants camera = ahead();
            camera.mTransparentBackground = 1;
            mRenderer->traceGuiTexture(texture, camera, GuiTraceOptions{ .mScene = doll, .mReadBack = true });

            // Given back with the picture still deferred, and the world drawn on as if nothing
            // happened: three frames, which is more than the ring holds, so the scene's submit has
            // been waited out by the end of them and the scene has gone.
            mRenderer->dropViewScene(doll);
            for (int frame = 0; frame < 3; ++frame)
            {
                moveTo(-1000.0f + 100.0f * static_cast<float>(frame));
                mRenderer->renderFrame(ahead(), FrameOptions{});
                mRenderer->collectFrame();
            }
            mRenderer->finishGuiTraces();

            ASSERT_TRUE(mRenderer->takeGuiCopy(texture, copy)) << "the picture the drop left deferred never landed";
            EXPECT_EQ(copy[3], 255) << "the picture was traced against a scene that had gone";

            while (mRenderer->finishFrame().has_value())
            {
            }

            mRenderer->dropGuiTexture(texture);
        }
    }
}
