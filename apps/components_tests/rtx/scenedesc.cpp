#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Vec3f>

#include <components/rtx/error.hpp>
#include <components/rtx/instancerecord.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/spritelight.hpp>

#include "geometry.hpp"

namespace Rtx
{
    namespace
    {
        /// The runs a list names, as a vector a matcher can compare.
        std::vector<Rtx::Run> runs(std::span<const Rtx::Run> spans)
        {
            return std::vector<Rtx::Run>(spans.begin(), spans.end());
        }

        /// What a news list names, sorted, so a set can be compared without depending on the order
        /// the sweep happened to walk its table in.
        std::vector<Index> sorted(std::span<const Index> slots)
        {
            std::vector<Index> copy(slots.begin(), slots.end());
            std::sort(copy.begin(), copy.end());
            return copy;
        }

        /// The order the lights come out in is the lights' own, and every field takes its turn.
        ///
        /// **What a repeated run rests on.** `orderLights` says why: a walk meets lights in graph
        /// order, a graph gains and loses cells as a player moves, and the grid and the reservoir
        /// both read the order — so the same place walked twice draws a different picture unless
        /// this is a total order over what a light *is*.
        ///
        /// **Every step below raises exactly one field and leaves every earlier one alone**, which
        /// is what makes a field dropped from the comparator show: the two rows it separates become
        /// equal, and they were handed over in the opposite order. A step that raised two at once
        /// would be ordered by whichever of them survived. The four rows at the front are the same
        /// statement about `osg::Vec3f`, whose order is lexicographic on x, y and z.
        TEST(RtxSceneDescTest, everyFieldOfALightTakesItsTurnInTheOrder)
        {
            const osg::Vec3f one{ 1.0f, 1.0f, 1.0f };

            // Ascending, and each row names only what it raises: everything else a `Light` carries
            // starts at nothing.
            const std::array<Light, 9> ordered{
                Light{},
                Light{ .mPosition = { 0.0f, 0.0f, 1.0f } },
                Light{ .mPosition = { 0.0f, 1.0f, 0.0f } },
                Light{ .mPosition = { 1.0f, 0.0f, 0.0f } },
                Light{ .mPosition = one },
                Light{ .mPosition = one, .mIntensity = one },
                Light{ .mPosition = one, .mIntensity = one, .mReach = 1.0f },
                Light{ .mPosition = one, .mIntensity = one, .mReach = 1.0f, .mSourceRadius = 1.0f },
                Light{ .mPosition = one, .mIntensity = one, .mReach = 1.0f, .mSourceRadius = 1.0f, .mClearance = 1.0f },
            };

            // Handed over backwards, so a walk that did nothing at all would fail this.
            SceneDesc scene;
            for (auto light = ordered.rbegin(); light != ordered.rend(); ++light)
                scene.addLight(*light);

            scene.orderLights();

            ASSERT_EQ(scene.lights().size(), ordered.size());
            for (std::size_t at = 0; at < ordered.size(); ++at)
            {
                const Light& made = scene.lights()[at];
                EXPECT_EQ(made.mPosition, ordered[at].mPosition) << "position at " << at;
                EXPECT_EQ(made.mIntensity, ordered[at].mIntensity) << "intensity at " << at;
                EXPECT_EQ(made.mReach, ordered[at].mReach) << "reach at " << at;
                EXPECT_EQ(made.mSourceRadius, ordered[at].mSourceRadius) << "source radius at " << at;
                EXPECT_EQ(made.mClearance, ordered[at].mClearance) << "clearance at " << at;
            }
        }

        TEST(RtxSceneDescTest, aMeshRemembersWhereItsVerticesWent)
        {
            SceneDesc scene;

            const Index first
                = scene.addMesh(MeshArrays{ .mPositions = Testing::sUnitQuad, .mIndices = Testing::sQuadIndices });
            const Index second
                = scene.addMesh(MeshArrays{ .mPositions = Testing::sUnitQuad, .mIndices = Testing::sQuadIndices });

            EXPECT_EQ(first, 0u);
            EXPECT_EQ(second, 1u);

            // Two quads: 8 vertices and 12 indices in the shared buffers, the second mesh starting
            // where the first left off.
            EXPECT_EQ(scene.meshes().getPositions().size(), 8u);
            EXPECT_EQ(scene.meshes().getIndices().size(), 12u);
            EXPECT_EQ(scene.meshes().getRows()[1].mVertices.mOffset, 4u);
            EXPECT_EQ(scene.meshes().getRows()[1].mIndices.mOffset, 6u);

            EXPECT_EQ(scene.meshes().getMeshPositions(second)[2], osg::Vec3f(1.0f, 1.0f, 0.0f));
            EXPECT_EQ(scene.meshes().getMeshIndices(second)[5], 3u);

            // And whether the caller found it doubled for its back, which the scene keeps and
            // never works out for itself.
            EXPECT_FALSE(scene.meshes().getRows()[first].mShape.mSheet);

            // Added first and read after: the table grows under a span taken in the same expression.
            const Index sheet
                = scene.addMesh(MeshArrays{ .mPositions = Testing::sUnitQuad, .mIndices = Testing::sQuadIndices },
                    FoldedShape{ .mSheet = true });
            EXPECT_TRUE(scene.meshes().getRows()[sheet].mShape.mSheet);
        }

        /// A mesh without normals or texture coordinates must still leave the attribute buffers as
        /// long as the position buffer, or every vertex after it reads someone else's normal.
        TEST(RtxSceneDescTest, theAttributeBuffersStayParallelWhenAMeshBringsNoAttributes)
        {
            SceneDesc scene;

            const std::array sNormals{
                osg::Vec3f(0.0f, 0.0f, 1.0f),
                osg::Vec3f(0.0f, 0.0f, 1.0f),
                osg::Vec3f(0.0f, 0.0f, 1.0f),
                osg::Vec3f(0.0f, 0.0f, 1.0f),
            };

            scene.addMesh(MeshArrays{ .mPositions = Testing::sUnitQuad, .mIndices = Testing::sQuadIndices });
            const Index withNormals = scene.addMesh(MeshArrays{
                .mPositions = Testing::sUnitQuad, .mNormals = sNormals, .mIndices = Testing::sQuadIndices });

            const Rtx::MeshTable& meshes = scene.meshes();
            ASSERT_EQ(meshes.getNormals().size(), meshes.getPositions().size());
            ASSERT_EQ(meshes.getTexCoords().size(), meshes.getPositions().size());

            const MeshRange& range = scene.meshes().getRows()[withNormals];
            EXPECT_EQ(scene.meshes().getNormals()[range.mVertices.mOffset], osg::Vec3f(0.0f, 0.0f, 1.0f));
            EXPECT_EQ(scene.meshes().getNormals()[0], osg::Vec3f(0.0f, 0.0f, 0.0f));
        }

        TEST(RtxSceneDescTest, aTextureIsAddedOnceHoweverOftenItIsAskedFor)
        {
            SceneDesc scene;

            constexpr VFS::Path::NormalizedView stone("textures/tx_stone_01.dds");
            constexpr VFS::Path::NormalizedView wood("textures/tx_wood_01.dds");

            EXPECT_EQ(scene.textures().add(stone), 0u);
            EXPECT_EQ(scene.textures().add(wood), 1u);
            EXPECT_EQ(scene.textures().add(stone), 0u);
            EXPECT_EQ(scene.textures().getPaths().size(), 2u);
        }

        /// **Which slot a thing lands in cannot depend on the order the dead left in.**
        /// `Rtx::Identity` hashes by address, so a sweep gives slots back in whatever order the
        /// allocator left its map in — and a table that answered with the last one freed then handed
        /// one live set two different layouts in two processes. Measured on `one-cell-walk` before
        /// this: the `materials` and `textures` columns of the hashes table differed from frame 2 on
        /// 89 frames of 90, and the picture followed at frame 39.
        ///
        /// **Both orders on one fixture**, because "the lowest" and "the last freed" agree wherever
        /// the frees happen to run upwards.
        TEST(RtxSceneDescTest, theSlotHandedOutIsTheSameHoweverTheSlotsWereGivenBack)
        {
            constexpr std::array<VFS::Path::NormalizedView, 4> named{
                VFS::Path::NormalizedView("textures/tx_a.dds"),
                VFS::Path::NormalizedView("textures/tx_b.dds"),
                VFS::Path::NormalizedView("textures/tx_c.dds"),
                VFS::Path::NormalizedView("textures/tx_d.dds"),
            };

            const auto after = [&](const std::vector<Index>& order) {
                SceneDesc scene;
                for (const VFS::Path::NormalizedView path : named)
                    scene.textures().hold(scene.textures().add(path));

                for (const Index slot : order)
                    scene.textures().drop(slot);

                return std::array<Index, 3>{ scene.textures().add(VFS::Path::NormalizedView("textures/tx_e.dds")),
                    scene.textures().add(VFS::Path::NormalizedView("textures/tx_f.dds")),
                    scene.textures().add(VFS::Path::NormalizedView("textures/tx_g.dds")) };
            };

            const std::array<Index, 3> expected{ 0u, 2u, 3u };
            EXPECT_EQ(after({ 0u, 2u, 3u }), expected) << "given back lowest first";
            EXPECT_EQ(after({ 3u, 2u, 0u }), expected) << "given back highest first";
            EXPECT_EQ(after({ 2u, 0u, 3u }), expected) << "given back in no order at all";
        }

        TEST(RtxSceneDescTest, theCountsAreWhatTheBuffersHold)
        {
            SceneDesc scene;
            scene.addMesh(MeshArrays{ .mPositions = Testing::sUnitQuad, .mIndices = Testing::sQuadIndices });
            scene.addMesh(MeshArrays{ .mPositions = Testing::sUnitQuad, .mIndices = Testing::sQuadIndices });

            EXPECT_EQ(scene.meshes().getTriangleCount(), 4u);
            EXPECT_EQ(scene.meshes().getRows()[0].getTriangleCount(), 2u);

            // 8 positions, 8 normals and 8 colours at 12 bytes, 8 texture coordinates at 8, and 12
            // indices at 4. The mesh brought neither normal, coordinate nor colour and the buffers
            // hold one apiece regardless — `MeshTable::writeAttributes` says why.
            EXPECT_EQ(scene.meshes().getGeometryBytes(), 8u * 12u + 8u * 12u + 8u * 8u + 8u * 12u + 12u * 4u);
        }

        /// The cutoff a material is traced against, and which materials get traced against one.
        ///
        /// The blended case is the load-bearing one: Morrowind's foliage is drawn with
        /// `NiAlphaProperty` and no alpha test, so a renderer that only honoured the tested mode
        /// would find nothing to cut out. A blend that *did* name a threshold keeps its own.
        TEST(RtxSceneDescTest, onlyAMaterialWithAMaskToReadIsTracedAsACutout)
        {
            constexpr Index texture = 3;

            const Material opaque{ .mDiffuse = texture };
            EXPECT_EQ(opaque.getAlphaCutoff(), 0.0f);
            EXPECT_FALSE(opaque.isCutout());

            const Material tested{ .mDiffuse = texture, .mAlphaRef = 0.3f, .mAlphaMode = Surface::AlphaMode::Cutout };
            EXPECT_EQ(tested.getAlphaCutoff(), 0.3f);
            EXPECT_TRUE(tested.isCutout());

            const Material blended{ .mDiffuse = texture, .mAlphaMode = Surface::AlphaMode::Blend };
            EXPECT_EQ(blended.getAlphaCutoff(), 0.5f);
            EXPECT_TRUE(blended.isCutout());

            const Material blendedWithRef{
                .mDiffuse = texture, .mAlphaRef = 0.8f, .mAlphaMode = Surface::AlphaMode::Blend
            };
            EXPECT_EQ(blendedWithRef.getAlphaCutoff(), 0.8f);

            // The mask lives in the diffuse map's alpha, so a cutoff with no map to read it from is
            // not a cutout — and marking it one would cost traversal a candidate loop that could
            // only ever say yes.
            const Material untextured{ .mAlphaMode = Surface::AlphaMode::Blend };
            EXPECT_EQ(untextured.getAlphaCutoff(), 0.5f);
            EXPECT_FALSE(untextured.isCutout());
        }

        /// A leaf card and a pane of glass carry the same alpha mode, and the material's own alpha is
        /// what tells them apart.
        ///
        /// **The mode says nothing about it**, because Morrowind keeps its foliage under
        /// `NiAlphaProperty`: a leaf is fully opaque wherever its painted mask is, and a pane is
        /// translucent everywhere. The two want opposite answers from traversal — a mask averaged
        /// over the ray cone and tested is right for the leaf and turns the pane solid; light
        /// attenuated as it passes is right for the pane and turns the leaf to gauze — so nothing may
        /// act on the mode alone.
        ///
        /// `NiMaterialProperty` records that alpha and `NifOsg::AlphaController` animates it, so a
        /// surface can cross this line while the game runs.
        TEST(RtxSceneDescTest, theMaterialsOwnAlphaIsWhatTellsAPaneOfGlassFromALeaf)
        {
            constexpr Index texture = 3;

            const Material leaf{ .mDiffuse = texture, .mAlphaMode = Surface::AlphaMode::Blend };
            EXPECT_FALSE(leaf.isTranslucent()) << "a painted mask on an opaque material";
            EXPECT_TRUE(leaf.isCutout()) << "and it keeps the branch it has";

            const Material pane{ .mDiffuse = texture, .mOpacity = 0.3f, .mAlphaMode = Surface::AlphaMode::Blend };
            EXPECT_TRUE(pane.isTranslucent());

            // The mode is half of it: a faded material the content never asked to blend is drawn as
            // it was authored, and a cutout stays a cutout however faint its own alpha is.
            const Material faded{ .mDiffuse = texture, .mOpacity = 0.3f };
            EXPECT_FALSE(faded.isTranslucent()) << "opaque mode, whatever the alpha says";

            const Material tested{
                .mDiffuse = texture, .mOpacity = 0.3f, .mAlphaRef = 0.3f, .mAlphaMode = Surface::AlphaMode::Cutout
            };
            EXPECT_FALSE(tested.isTranslucent()) << "a mask the content asked to test is a mask";

            // And the texture is the other half of what tells a pane from a cloud. Neither of them
            // is a medium on its own answer: the leaf keeps its mask whatever its paint does, and
            // the pane stays a surface while its paint closes anywhere.
            EXPECT_FALSE(leaf.isMedium());
            EXPECT_FALSE(pane.isMedium());
        }

        /// A medium is a translucent material whose paint never closes, and it takes both.
        ///
        /// **The two facts are independent and neither implies the other.** A leaf card carries paint
        /// that reaches solid and a material that does not blend, so it stays a mask however it is
        /// marked. A pane of stained glass blends and has lead came in it, so something still stops
        /// on it. A cloud has neither, and a ray goes through it.
        ///
        /// **And a material with no diffuse map at all is a surface**, which is an untextured pane:
        /// all glass, no paint, and a thing to stop on wherever it stands.
        TEST(RtxSceneDescTest, aMediumIsBlendedEverywhereAndPaintedSolidNowhere)
        {
            constexpr Index texture = 3;
            constexpr float faint = 0.3f;

            const Material cloud{ .mDiffuse = texture,
                .mOpacity = faint,
                .mAlphaMode = Surface::AlphaMode::Blend,
                .mDiffuseNeverSolid = true };
            EXPECT_TRUE(cloud.isMedium());
            EXPECT_TRUE(cloud.getTraversed().mMedium) << "and the placements wearing it are told";

            const Material stained{ .mDiffuse = texture, .mOpacity = faint, .mAlphaMode = Surface::AlphaMode::Blend };
            EXPECT_FALSE(stained.isMedium()) << "paint that closes is something to stop on";

            const Material leaf{
                .mDiffuse = texture, .mAlphaMode = Surface::AlphaMode::Blend, .mDiffuseNeverSolid = true
            };
            EXPECT_FALSE(leaf.isMedium()) << "an opaque material, whatever its paint does";

            const Material glass{ .mOpacity = faint, .mAlphaMode = Surface::AlphaMode::Blend };
            EXPECT_FALSE(glass.isMedium()) << "no map to have measured";
        }

        /// The row a bone standing `z` up carries: the identity's three rows with the translation
        /// in the last column of the third.
        Shaders::GpuBone boneUp(float z)
        {
            return Shaders::GpuBone{ .mRows = { osg::Vec4f(1.0f, 0.0f, 0.0f, 0.0f), osg::Vec4f(0.0f, 1.0f, 0.0f, 0.0f),
                                         osg::Vec4f(0.0f, 0.0f, 1.0f, z) } };
        }

        /// One rig with a still mesh beside two skinned ones, which is the shape all three tests
        /// below are about.
        ///
        /// **The still mesh is what makes them worth running**: rows written at the wrong offset
        /// would land in a neighbour's, and the bind run of a deforming mesh is a table of its own
        /// that a static neighbour must not be in.
        class RtxSkinnedMeshTest : public ::testing::Test
        {
        protected:
            /// An upward normal per corner, so a pose that rewrote one would be read.
            static std::array<osg::Vec3f, 4> upward()
            {
                return {
                    osg::Vec3f(0.0f, 0.0f, 1.0f),
                    osg::Vec3f(0.0f, 0.0f, 1.0f),
                    osg::Vec3f(0.0f, 0.0f, 1.0f),
                    osg::Vec3f(0.0f, 0.0f, 1.0f),
                };
            }

            Index addSkin() { return addQuad(Deform::Rig, mRig); }

            SceneDesc mScene;
            Index mRig = Testing::addOneBoneRig(mScene, 4);
            Index mStill = addQuad(Deform::None, sNoIndex);
            Index mMoving = addSkin();
            Index mOther = addSkin();

            const std::array<Shaders::GpuBone, 1> mAtFive{ boneUp(5.0f) };
            const std::array<Shaders::GpuBone, 1> mAtSeven{ boneUp(7.0f) };
            const osg::BoundingBoxf mReach{ osg::Vec3f(0.0f, 0.0f, 5.0f), osg::Vec3f(1.0f, 1.0f, 5.0f) };

        private:
            Index addQuad(Deform deform, Index deformer)
            {
                return mScene.addMesh(
                    MeshArrays{
                        .mPositions = Testing::sUnitQuad, .mNormals = upward(), .mIndices = Testing::sQuadIndices },
                    {}, deform, deformer);
            }
        };

        /// A rig and the meshes on it arrive with the tables they name, and the still one is in none
        /// of them.
        TEST_F(RtxSkinnedMeshTest, aRigAndTheMeshesOnItArriveWithTheTablesTheyName)
        {
            EXPECT_TRUE(mScene.meshes().getDeformed().empty()) << "nothing has been posed yet";

            // The rig's tables: four run words and one influence, and one bone per mesh on it.
            ASSERT_EQ(mScene.deformers().getRigs().size(), 1u);
            EXPECT_EQ(mScene.deformers().getRigs()[mRig].getVertexCount(), 4u);
            EXPECT_EQ(mScene.deformers().getRigs()[mRig].mBoneCount, 1u);
            EXPECT_EQ(mScene.deformers().getRigHolds(mRig), 2u);
            EXPECT_EQ(mScene.deformers().getRuns().size(), 4u);
            EXPECT_EQ(mScene.deformers().getInfluences().size(), 1u);
            EXPECT_EQ(mScene.deformers().getArrivedRigs().size(), 1u);

            // The still mesh has no bind run and no rows; the two skinned ones have one apiece,
            // laid end to end.
            EXPECT_EQ(mScene.meshes().getRows()[mStill].mDeform, Deform::None);
            EXPECT_EQ(mScene.meshes().getRows()[mStill].mDeformer, sNoIndex);
            EXPECT_EQ(mScene.meshes().getRows()[mMoving].mDeform, Deform::Rig);
            EXPECT_EQ(mScene.meshes().getRows()[mMoving].mDeformer, mRig);
            EXPECT_EQ(mScene.meshes().getRows()[mMoving].mBindOffset, 0u);
            EXPECT_EQ(mScene.meshes().getRows()[mOther].mBindOffset, 4u);
            EXPECT_EQ(mScene.deformers().getBindVertexCount(), 8u) << "the bind table holds the skinned meshes alone";
            EXPECT_EQ(mScene.meshes().getRows()[mMoving].mPoseOffset, 0u);
            EXPECT_EQ(mScene.meshes().getRows()[mOther].mPoseOffset, 1u);
            EXPECT_EQ(mScene.deformers().getBones().size(), 2u);
        }

        /// A pose is rows and never vertices, and it names its mesh once a frame it moves.
        TEST_F(RtxSkinnedMeshTest, aPoseNamesItsMeshOncePerFrameAndLeavesEveryVertexAlone)
        {
            // **The first pose names the mesh whatever it is**, and a second in the same frame is
            // the same structure to refit.
            mScene.poseRig(mMoving, mAtFive, mReach);
            mScene.poseRig(mMoving, mAtFive, mReach);

            ASSERT_EQ(mScene.meshes().getDeformed().size(), 1u) << "twice in a frame is one structure to refit";
            EXPECT_EQ(mScene.meshes().getDeformed()[0], mMoving);
            EXPECT_EQ(mScene.getMeshBones(mMoving)[0].mRows[2], osg::Vec4f(0.0f, 0.0f, 1.0f, 5.0f));
            EXPECT_EQ(mScene.meshes().getRows()[mMoving].mBounds, mReach)
                << "the reach is the caller's and not the bind's";

            // The bind pose stays where it arrived, and so does everything beside it.
            EXPECT_EQ(mScene.meshes().getPositions().size(), 12u);
            EXPECT_EQ(mScene.meshes().getRows()[mMoving].mVertices.mOffset, 4u);
            EXPECT_EQ(mScene.meshes().getMeshPositions(mMoving)[2], osg::Vec3f(1.0f, 1.0f, 0.0f));
            EXPECT_EQ(mScene.meshes().getMeshPositions(mStill)[2], osg::Vec3f(1.0f, 1.0f, 0.0f));
            EXPECT_EQ(mScene.getMeshBones(mOther)[0], Shaders::GpuBone{}) << "the neighbour's rows are untouched";

            // The list is a frame's worth, so it goes when the frame's placements do.
            mScene.clearPlacement();
            EXPECT_TRUE(mScene.meshes().getDeformed().empty());
            EXPECT_EQ(mScene.meshes().getRows().size(), 3u) << "clearing where things are keeps what they are";

            // **A pose that did not change names nothing.** The walk poses every rig it meets and
            // cannot tell which of them the engine animated; the scene can, by looking.
            mScene.poseRig(mMoving, mAtFive, mReach);
            EXPECT_TRUE(mScene.meshes().getDeformed().empty()) << "an unchanged pose named a structure to refit";

            mScene.poseRig(mMoving, mAtSeven, mReach);
            mScene.poseRig(mOther, mAtFive, mReach);
            EXPECT_EQ(sorted(mScene.meshes().getDeformed()), (std::vector<Index>{ mMoving, mOther }));
            EXPECT_EQ(mScene.getMeshBones(mMoving)[0].mRows[2], osg::Vec4f(0.0f, 0.0f, 1.0f, 7.0f));
            EXPECT_EQ(mScene.getMeshBones(mOther)[0].mRows[2], osg::Vec4f(0.0f, 0.0f, 1.0f, 5.0f));
        }

        /// **The rig goes with the last mesh on it, and not before.** Freeing one of the two gives
        /// its bind run and its rows back and leaves the rig standing; freeing the other frees the
        /// rig, and the next skin to arrive takes its slot and its runs.
        TEST_F(RtxSkinnedMeshTest, aRigGoesWithTheLastMeshOnItAndTheNextSkinTakesItsSlot)
        {
            mScene.poseRig(mMoving, mAtFive, mReach);
            mScene.poseRig(mOther, mAtFive, mReach);
            mScene.clearArrivals();

            const std::array keepTwo{ mStill, mOther };
            ASSERT_TRUE(mScene.release(keepTwo, {}));
            EXPECT_EQ(mScene.deformers().getRigHolds(mRig), 1u);
            EXPECT_EQ(mScene.deformers().getRigs()[mRig].getVertexCount(), 4u) << "a rig with a mesh on it stays";
            EXPECT_EQ(std::vector<Index>(mScene.meshes().getDeformed().begin(), mScene.meshes().getDeformed().end()),
                (std::vector<Index>{ mOther }))
                << "the freed slot left the list and the survivor stayed where it was named";

            const std::array keepOne{ mStill };
            ASSERT_TRUE(mScene.release(keepOne, {}));
            EXPECT_EQ(mScene.deformers().getRigHolds(mRig), 0u);
            EXPECT_EQ(mScene.deformers().getRigs()[mRig].getVertexCount(), 0u) << "a rig nothing stands on is free";
            EXPECT_TRUE(mScene.deformers().getArrivedRigs().empty());
            EXPECT_TRUE(mScene.meshes().getDeformed().empty()) << "a slot given back still named a structure to refit";

            EXPECT_EQ(Testing::addOneBoneRig(mScene, 4), mRig) << "the freed slot is the one handed out";
            EXPECT_EQ(mScene.deformers().getRuns().size(), 4u) << "the freed run is the one handed out";
            EXPECT_EQ(sorted(mScene.deformers().getArrivedRigs()), (std::vector<Index>{ mRig }));

            // `mMoving` and not `mOther`, though `mOther` went last: `Rtx::SlotRows` answers with
            // the lowest free slot, so which of the two arrives next is not the sweep's to decide.
            const Index back = addSkin();
            EXPECT_EQ(back, mMoving) << "the freed mesh slot is the one handed out";
            EXPECT_EQ(mScene.meshes().getRows()[back].mBindOffset, 0u) << "the freed bind run is the one handed out";
            EXPECT_EQ(mScene.deformers().getBindVertexCount(), 4u)
                << "both runs went, so the table reaches only as far as this one";
            EXPECT_EQ(mScene.getMeshBones(back)[0], Shaders::GpuBone{}) << "a reused pose run holds no old pose";

            mScene.poseRig(back, mAtFive, mReach);
            EXPECT_EQ(sorted(mScene.meshes().getDeformed()), (std::vector<Index>{ back }))
                << "a reused slot's first pose names it";
        }

        /// **A rig slot goes back onto a heap and not onto a stack**, which the test above cannot
        /// tell: it frees one slot, and one slot is the same answer either way.
        ///
        /// A sweep frees a rig where the last mesh on it goes, so rigs are given back in their
        /// *meshes'* order and not in their own. Here the mesh on the second rig is the first mesh,
        /// so the second rig is freed first and the free list is handed `1` and then `0` — which a
        /// list built by pushing leaves out of order. `Rtx::SlotRows` answers with the lowest,
        /// so the next rig has to land in slot 0; a stack would answer with slot 1.
        TEST(RtxSceneDescTest, aFreedRigSlotIsHandedOutLowestFirstHoweverTheSweepMetIt)
        {
            SceneDesc scene;

            const Index first = Testing::addOneBoneRig(scene, 4);
            const Index second = Testing::addOneBoneRig(scene, 4);
            ASSERT_EQ(first, 0u);
            ASSERT_EQ(second, 1u);

            const auto addSkin = [&](const Index rig) {
                return scene.addMesh(MeshArrays{ .mPositions = Testing::sUnitQuad, .mIndices = Testing::sQuadIndices },
                    {}, Deform::Rig, rig);
            };

            // The mesh on the second rig is the lower mesh slot, which is what makes the sweep free
            // the two rigs in the order that catches this.
            const Index early = addSkin(second);
            const Index late = addSkin(first);
            ASSERT_LT(early, late);

            ASSERT_TRUE(scene.release({}, {}));
            ASSERT_EQ(scene.deformers().getRigHolds(first), 0u);
            ASSERT_EQ(scene.deformers().getRigHolds(second), 0u);

            // **Read after two removals in one sweep**, which is the pass `DeformerTable::compact`
            // owes: a set with a removal outstanding refuses to answer at all.
            EXPECT_TRUE(scene.deformers().getArrivedRigs().empty()) << "both arrivals left with their rigs";

            EXPECT_EQ(Testing::addOneBoneRig(scene, 4), first)
                << "the lowest free rig slot, and not the last one the sweep gave back";
            EXPECT_EQ(Testing::addOneBoneRig(scene, 4), second) << "then the one above it";
        }

        /// **Every table hands out its lowest free slot**, which is what `Rtx::SlotRows` promises
        /// once for all six of them.
        ///
        /// Two slots are freed high first here, because that is the order a list built by pushing
        /// leaves out of order: `[2]` and then `[2, 0]` is no heap, and a pop of it answers with 2.
        /// A slot is the custom index a hit reads back and the row a material is looked up in, so
        /// which of two free slots an arrival takes has to be a function of what is standing.
        TEST(RtxSceneDescTest, everyTableHandsOutItsLowestFreeSlot)
        {
            SceneDesc scene;

            const auto quad = [&](const Index material) {
                return scene.addMesh(MeshArrays{ .mPositions = Testing::sUnitQuad, .mIndices = Testing::sQuadIndices },
                    {}, Deform::None, sNoIndex, material);
            };

            const std::array meshes{ quad(sNoIndex), quad(sNoIndex), quad(sNoIndex) };
            ASSERT_EQ(meshes[2], 2u);

            const std::array materials{ scene.materials().add(Material{ .mAlphaRef = 0.25f }),
                scene.materials().add(Material{ .mAlphaRef = 0.5f }),
                scene.materials().add(Material{ .mAlphaRef = 0.75f }) };
            ASSERT_EQ(materials[2], 2u);

            const auto path = [](const char* name) { return VFS::Path::NormalizedView(name); };
            const std::array textures{ scene.textures().add(path("textures/a.dds")),
                scene.textures().add(path("textures/b.dds")), scene.textures().add(path("textures/c.dds")) };
            for (const Index texture : textures)
                scene.textures().hold(texture);
            ASSERT_EQ(textures[2], 2u);

            const auto place = [&](const Index mesh) { return scene.addInstance(MeshInstance{ .mMesh = mesh }); };
            const std::array placed{ place(meshes[0]), place(meshes[1]), place(meshes[2]) };
            ASSERT_EQ(placed[2], 2u);

            // The highest of each three first, and the lowest second.
            scene.textures().drop(textures[2]);
            scene.textures().drop(textures[0]);

            scene.placements().drop(placed[2]);
            scene.placements().drop(placed[0]);

            const std::array keepTwo{ meshes[0], meshes[1] };
            const std::array keepTwoMaterials{ materials[0], materials[1] };
            ASSERT_TRUE(scene.release(keepTwo, keepTwoMaterials));

            const std::array keepOne{ meshes[1] };
            const std::array keepOneMaterial{ materials[1] };
            ASSERT_TRUE(scene.release(keepOne, keepOneMaterial));

            EXPECT_EQ(scene.textures().add(path("textures/d.dds")), textures[0]) << "textures";
            EXPECT_EQ(place(meshes[1]), placed[0]) << "placements";
            EXPECT_EQ(quad(sNoIndex), meshes[0]) << "meshes";
            EXPECT_EQ(scene.materials().add(Material{ .mAlphaRef = 0.125f }), materials[0]) << "materials";
        }

        /// A morphed mesh holds its base as its bind pose and its weights as its pose, and the
        /// offsets of every target laid end to end beside it.
        ///
        /// Hand-counted: two targets over four vertices is eight offsets, the base's four zeroes
        /// first; a pose is two weights, of which the base's is carried and never read.
        TEST(RtxSceneDescTest, aMorphedMeshHoldsItsTargetsAndNamesItselfOncePerPose)
        {
            SceneDesc scene;

            std::array<osg::Vec3f, 8> offsets{};
            for (std::size_t vertex = 4; vertex < 8; ++vertex)
                offsets[vertex] = osg::Vec3f(0.0f, 0.0f, 1.0f);

            const Index morph = scene.deformers().addMorph(offsets, 2);
            ASSERT_EQ(scene.deformers().getMorphs().size(), 1u);
            EXPECT_EQ(scene.deformers().getMorphs()[morph].mTargetCount, 2u);
            EXPECT_EQ(scene.deformers().getMorphs()[morph].getVertexCount(), 4u);
            EXPECT_EQ(scene.deformers().getMorphOffsets().size(), 8u);
            EXPECT_EQ(scene.deformers().getMorphOffsets()[6], osg::Vec3f(0.0f, 0.0f, 1.0f));
            EXPECT_EQ(sorted(scene.deformers().getArrivedMorphs()), (std::vector<Index>{ morph }));

            const Index face
                = scene.addMesh(MeshArrays{ .mPositions = Testing::sUnitQuad, .mIndices = Testing::sQuadIndices }, {},
                    Deform::Morph, morph);
            EXPECT_EQ(scene.meshes().getRows()[face].mDeform, Deform::Morph);
            EXPECT_EQ(scene.deformers().getMorphHolds(morph), 1u);
            EXPECT_EQ(scene.deformers().getWeights().size(), 2u);
            EXPECT_EQ(scene.deformers().getBindVertexCount(), 4u);

            const std::array smiling{ 1.0f, 0.5f };
            const osg::BoundingBoxf reach(osg::Vec3f(0.0f, 0.0f, 0.0f), osg::Vec3f(1.0f, 1.0f, 0.5f));
            scene.poseMorph(face, smiling, reach);
            scene.poseMorph(face, smiling, reach);
            EXPECT_EQ(sorted(scene.meshes().getDeformed()), (std::vector<Index>{ face }));
            EXPECT_EQ(scene.getMeshWeights(face)[1], 0.5f);
            EXPECT_EQ(scene.meshes().getRows()[face].mBounds, reach);

            scene.clearPlacement();
            scene.poseMorph(face, smiling, reach);
            EXPECT_TRUE(scene.meshes().getDeformed().empty()) << "an unchanged pose named a structure to refit";

            // The morph goes with its mesh and its offsets with it: the next set of the same shape
            // lands where they were.
            ASSERT_TRUE(scene.release({}, {}));
            EXPECT_EQ(scene.deformers().getMorphHolds(morph), 0u);
            EXPECT_EQ(scene.deformers().getMorphs()[morph].getVertexCount(), 0u);
            EXPECT_EQ(scene.deformers().addMorph(offsets, 2), morph);
            EXPECT_EQ(scene.deformers().getMorphOffsets().size(), 8u);
        }

        /// The finding the caller made about a mesh is kept beside its range, for a backend that
        /// builds a deforming mesh's structure to be refitted.
        TEST(RtxSceneDescTest, aMeshCarriesWhetherItDeformsAndWhatItArrivedWearing)
        {
            SceneDesc scene;
            const Index still
                = scene.addMesh(MeshArrays{ .mPositions = Testing::sUnitQuad, .mIndices = Testing::sQuadIndices });
            EXPECT_EQ(scene.meshes().getRows()[still].mDeform, Deform::None);
            EXPECT_EQ(scene.meshes().getRows()[still].mMaterial, sNoIndex);

            const Index rig
                = scene.addMesh(MeshArrays{ .mPositions = Testing::sUnitQuad, .mIndices = Testing::sQuadIndices }, {},
                    Deform::Rig, Testing::addOneBoneRig(scene, 4));
            EXPECT_EQ(scene.meshes().getRows()[rig].mDeform, Deform::Rig);

            // The material a mesh arrives wearing is kept as it was handed over, and a slot given
            // back forgets it with the rest of what stood there.
            const Index worn = scene.materials().add(Material{});
            const Index dressed
                = scene.addMesh(MeshArrays{ .mPositions = Testing::sUnitQuad, .mIndices = Testing::sQuadIndices }, {},
                    Deform::None, sNoIndex, worn);
            EXPECT_EQ(scene.meshes().getRows()[dressed].mMaterial, worn);

            const std::array<Index, 2> keptMeshes{ still, rig };
            const std::array<Index, 1> keptMaterials{ worn };
            ASSERT_TRUE(scene.release(keptMeshes, keptMaterials));
            EXPECT_EQ(scene.meshes().getRows()[dressed].mMaterial, sNoIndex);
            EXPECT_EQ(scene.meshes().getRows()[dressed].mVertices.mCount, 0u);
        }

        /// Every change to a placement's row is reported, and nothing else is.
        ///
        /// **What lets a backend rewrite hundreds of rows a frame and not tens of thousands.** The
        /// row carries the transform, the opacity and what traversal is told about the material, so
        /// each of those changing is a row; a texture scrolling under the same material is not. And
        /// what settled — the rows whose motion went back to nothing — is reported the frame after,
        /// or a backend would leave last frame's motion in a row for ever.
        TEST(RtxSceneDescTest, aRowIsReportedWhenAPlacementIsPlacedMovedFadedDroppedOrReclassed)
        {
            SceneDesc scene;
            const Index mesh
                = scene.addMesh(MeshArrays{ .mPositions = Testing::sUnitQuad, .mIndices = Testing::sQuadIndices });
            const Index glass = scene.materials().add(Material{
                .mOpacity = 0.5f,
                .mAlphaMode = Surface::AlphaMode::Blend,
            });

            const Index one = scene.addInstance(MeshInstance{ .mMesh = mesh, .mMaterial = glass });
            const Index two = scene.addInstance(MeshInstance{ .mMesh = mesh, .mMaterial = glass });
            EXPECT_EQ(sorted(scene.placements().getMoved()), (std::vector<Index>{ one, two }))
                << "a placement made is a row";
            EXPECT_TRUE(scene.placements().getSettled().empty());

            scene.placements().advance();
            EXPECT_TRUE(scene.placements().getMoved().empty());
            EXPECT_EQ(sorted(scene.placements().getSettled()), (std::vector<Index>{ one, two }))
                << "what moved is what settles";

            // A fade that changes the number is a row; one that does not is nothing. And the settled
            // list is the last moved list and nothing older.
            scene.placements().fade(one, 0.5f);
            scene.placements().fade(one, 0.5f);
            EXPECT_EQ(sorted(scene.placements().getMoved()), (std::vector<Index>{ one }));
            scene.placements().advance();
            EXPECT_EQ(sorted(scene.placements().getSettled()), (std::vector<Index>{ one }));

            // A material crossing opaque re-classes every placement wearing it; a texture scrolling
            // under it re-classes none.
            Material worn = scene.materials().getRows()[glass];
            worn.mTextureTransform = osg::Vec4f(1.0f, 1.0f, 0.25f, 0.0f);
            scene.setMaterial(glass, worn);
            EXPECT_TRUE(scene.placements().getMoved().empty())
                << "a texture scrolling reported the placements wearing it";

            worn.mOpacity = 1.0f;
            scene.setMaterial(glass, worn);
            EXPECT_EQ(sorted(scene.placements().getMoved()), (std::vector<Index>{ one, two }));
            scene.placements().advance();

            // A move is a row and a fade in the same frame is the same row twice, which is one row
            // written twice and not a wrong one.
            scene.placements().move(two, osg::Matrixf::translate(0.0f, 0.0f, 5.0f));
            scene.placements().fade(two, 0.25f);
            EXPECT_EQ(sorted(scene.placements().getMoved()), (std::vector<Index>{ two, two }));
            scene.placements().advance();

            // A dropped slot is a row to write inactive, and the slot it frees is the next
            // placement's — both reported, on the frames they happen.
            scene.placements().drop(two);
            EXPECT_EQ(sorted(scene.placements().getMoved()), (std::vector<Index>{ two }));
            scene.placements().advance();
            EXPECT_EQ(scene.addInstance(MeshInstance{ .mMesh = mesh }), two);
            EXPECT_EQ(sorted(scene.placements().getMoved()), (std::vector<Index>{ two }));

            // **An advance moves what was written into what settled, and leaves nothing behind
            // it.** A row still named as moved on the frame after it was written is a row a backend
            // writes twice, for ever.
            scene.placements().advance();
            EXPECT_TRUE(scene.placements().getMoved().empty());
            EXPECT_EQ(sorted(scene.placements().getSettled()), (std::vector<Index>{ two }));

            scene.placements().advance();
            EXPECT_TRUE(scene.placements().getSettled().empty());
        }

        /// An emitter's sphere is derived from the sprites rather than passed in, so the rejection
        /// test a ray makes and the sprites it would then walk cannot disagree about where they are.
        ///
        /// **Off the box and not off the mean**, which the lopsided arrangement here is chosen to
        /// prove: two sprites sit at the origin and one at four along x, so the mean is at 4/3 and
        /// the box's centre at 2. From the box the reach is 2 + 1 = 3 either way; from the mean it
        /// would have to be 8/3 + 1 = 3.67 to hold the far one, a sphere 22% wider for the same
        /// three particles.
        /// **Which slot an arrival takes is a fact about the world and never about the sweep.** A
        /// hit reads its slot back and a top-level structure is built in slot order, which is what
        /// settles a tie between two surfaces at one distance. The free list was a stack, so the
        /// slot followed the order the last sweep dropped in — and that order is a map walked in
        /// bucket order over keys hashed from node addresses.
        TEST(RtxSceneDescTest, theLowestFreeSlotIsTakenHoweverTheSlotsWereFreed)
        {
            const auto takeAfterDropping = [](const Index first, const Index second) {
                SceneDesc scene;
                const Index mesh
                    = scene.addMesh(MeshArrays{ .mPositions = Testing::sUnitQuad, .mIndices = Testing::sQuadIndices });

                for (Index at = 0; at < 5; ++at)
                    EXPECT_EQ(scene.addInstance(MeshInstance{ .mMesh = mesh }), at) << "a fresh table appends";

                scene.placements().drop(first);
                scene.placements().drop(second);

                std::array<Index, 3> taken{};
                for (Index& slot : taken)
                    slot = scene.addInstance(MeshInstance{ .mMesh = mesh });

                return taken;
            };

            // A stack answers with the slot dropped last — 1 one way round and 3 the other. The
            // lowest is 1 either way, then 3, and then a slot past the end once none is free.
            const std::array<Index, 3> expected{ 1, 3, 5 };
            EXPECT_EQ(takeAfterDropping(3, 1), expected) << "the higher slot freed first";
            EXPECT_EQ(takeAfterDropping(1, 3), expected) << "the lower slot freed first";
        }

        TEST(RtxSceneDescTest, anEmitterCarriesItsSpritesAndTheSphereThatHoldsThem)
        {
            SceneDesc scene;
            const Index texture = scene.textures().add(VFS::Path::NormalizedView("textures/tx_fire_00.dds"));

            // The bake of the texture's alpha sits in the same table, which is why the count of
            // textures at the end is two.
            const Index lighting = scene.textures().addBaked(
                SpriteLightMap::keyFor(VFS::Path::NormalizedView("textures/tx_fire_00.dds")));

            const std::array sPlume{
                Sprite{ .mPosition = osg::Vec3f(0.0f, 0.0f, 0.0f), .mRadius = 1.0f },
                Sprite{ .mPosition = osg::Vec3f(0.0f, 0.0f, 0.0f), .mRadius = 1.0f },
                Sprite{ .mPosition = osg::Vec3f(4.0f, 0.0f, 0.0f), .mRadius = 1.0f },
            };
            const std::array sSmoke{ Sprite{ .mPosition = osg::Vec3f(0.0f, 0.0f, 10.0f), .mRadius = 2.0f } };

            scene.addEmitter(sPlume, texture, true, 0.0f, lighting);
            ASSERT_EQ(scene.emitters().size(), 1u);

            // An emitter with nothing alive in it is not an emitter, and the next one that has
            // something starts where the first left off rather than where a placeholder would have.
            scene.addEmitter({}, texture, false);
            EXPECT_EQ(scene.emitters().size(), 1u);

            scene.addEmitter(sSmoke, texture, false);
            ASSERT_EQ(scene.emitters().size(), 2u);

            // Named once the adds are done, for the reason `SceneDesc`'s spans give.
            const std::span<const SpriteEmitter> made = scene.emitters();

            EXPECT_EQ(made[0].mCentre, osg::Vec3f(2.0f, 0.0f, 0.0f));
            EXPECT_FLOAT_EQ(made[0].mReach, 3.0f);
            EXPECT_EQ(made[0].mSprites, (Rtx::Run{ .mOffset = 0, .mCount = 3 }));
            EXPECT_EQ(made[0].mTexture, texture);
            EXPECT_EQ(made[0].mLighting, lighting);
            EXPECT_TRUE(made[0].mAdditive);

            EXPECT_EQ(made[1].mSprites, (Rtx::Run{ .mOffset = 3, .mCount = 1 }));
            EXPECT_FALSE(made[1].mAdditive) << "the blend the file asked for is what tells the two apart";
            EXPECT_EQ(made[1].mLighting, sNoIndex) << "an emitter with no bake is lit as a card";
            EXPECT_EQ(scene.sprites().size(), 4u);
            EXPECT_EQ(scene.sprites()[3].mPosition, osg::Vec3f(0.0f, 0.0f, 10.0f));

            // A frame's worth, so they go when the frame's placements do — and the texture they name
            // stays, because the array it indexes was uploaded when the scene was built.
            scene.clearPlacement();
            EXPECT_TRUE(scene.emitters().empty());
            EXPECT_TRUE(scene.sprites().empty());
            EXPECT_EQ(scene.textures().getPaths().size(), 2u);
        }

        /// A quad that hangs in the world reaches further than its own width, and its sphere knows.
        ///
        /// **Morrowind's rain is why `osgParticle` has a `FIXED` mode at all.** A billboard's axes
        /// are the screen's and it is a disc of one radius; a fixed one's are authored, and its
        /// *lengths* are the shape — rain's X is squashed to a tenth against a Y pointing straight
        /// down, which is a falling streak rather than a round drop.
        ///
        /// The reach has to be measured on that, and it is the one thing about the mode that a
        /// bounding sphere cannot guess: a streak ten times as tall as it is wide, measured on the
        /// width, is cut off nine tenths of the way up.
        TEST(RtxSceneDescTest, aFixedSpriteReachesByItsOwnAxesAndAnEyeFacingOneByItsRadius)
        {
            SceneDesc scene;
            const Index texture = scene.textures().add(VFS::Path::NormalizedView("textures/tx_raindrop_01.dds"));

            // Facing the eye: a disc, and the reach is the radius.
            const std::array disc{ Sprite{ .mPosition = osg::Vec3f(), .mRadius = 10.0f } };

            // Morrowind's own rain shape. The quad runs `+-0.1 * 10` across and `+-1 * 10` down, so
            // its corner is `|(0.1, 0, -1)| * 10 = 10.0499` from the middle — and that, not the ten,
            // is what has to fit in the sphere.
            const std::array streak{ Sprite{
                .mPosition = osg::Vec3f(), .mRadius = 10.0f, .mAxis = osg::Vec3f(0.0f, 0.0f, -1.0f) } };

            // The same streak leant by the wind, which is what the last claim below is measured on.
            const std::array leant{ Sprite{
                .mPosition = osg::Vec3f(), .mRadius = 10.0f, .mAxis = osg::Vec3f(0.0f, 0.5f, -0.8660254f) } };

            // **Every add before any read**, for the reason `SceneDesc`'s spans give: a row named
            // while another emitter is still to come is a row the next `addEmitter` moves out from
            // under the name.
            scene.addEmitter(disc, texture, false);
            scene.addEmitter(streak, texture, false, 0.1f);
            scene.addEmitter(leant, texture, false, 0.1f);

            ASSERT_EQ(scene.emitters().size(), 3u);
            const std::span<const SpriteEmitter> made = scene.emitters();

            EXPECT_FLOAT_EQ(made[0].mWidth, 0.0f) << "a width of nothing is a billboard";
            EXPECT_FLOAT_EQ(made[0].mReach, 10.0f);

            EXPECT_FLOAT_EQ(made[1].mWidth, 0.1f) << "carried as authored, because the length is the shape";
            EXPECT_NEAR(made[1].mReach, 10.0499f, 1e-3f);
            EXPECT_GT(made[1].mReach, 10.0f) << "further than the radius alone would have reached";

            // **And the axis is what the reach is measured on**, not the width beside it: a streak
            // leant by the wind reaches exactly as far as one falling straight down.
            EXPECT_NEAR(made[2].mReach, made[1].mReach, 1e-3f);
        }

        /// The unit quad lifted to `z`, so a mesh can be told apart by what came back out of it.
        std::array<osg::Vec3f, 4> quadAt(float z)
        {
            std::array<osg::Vec3f, 4> lifted = Testing::sUnitQuad;
            for (osg::Vec3f& vertex : lifted)
                vertex.z() = z;

            return lifted;
        }

        /// The unit triangle lifted the same way, so that a mesh beside the quads has a length of
        /// its own to be packed against.
        std::array<osg::Vec3f, 3> triangleAt(float z)
        {
            std::array<osg::Vec3f, 3> lifted = Testing::sUnitTriangle;
            for (osg::Vec3f& vertex : lifted)
                vertex.z() = z;

            return lifted;
        }

        /// A freed mesh keeps its index, and the room it held goes back for the next mesh to take.
        ///
        /// Hand-counted throughout. Three meshes of 4, 3 and 4 vertices sit at vertex offsets 0, 4
        /// and 7 and index offsets 0, 6 and 9. Freeing the middle one moves nothing: the third is
        /// still index 2 at vertex 7, and the hole at vertex 4 is three vertices and three indices
        /// wide — which is exactly a triangle, and exactly what the next triangle takes.
        ///
        /// **Not compacting is the whole point.** Closing the gap renames every mesh above it, and a
        /// mesh index is what every bottom-level acceleration structure in the world is named by, so
        /// a cell boundary cost a full rebuild.
        TEST(RtxSceneDescTest, aFreedMeshKeepsItsSlotAndTheNextThatFitsTakesIt)
        {
            SceneDesc scene;
            const std::array quads{ quadAt(0.0f), quadAt(2.0f) };
            const Index first = scene.addMesh(MeshArrays{ .mPositions = quads[0], .mIndices = Testing::sQuadIndices });
            const Index middle
                = scene.addMesh(MeshArrays{ .mPositions = triangleAt(5.0f), .mIndices = Testing::sTriangleIndices });
            const Index last = scene.addMesh(MeshArrays{ .mPositions = quads[1], .mIndices = Testing::sQuadIndices });

            ASSERT_EQ(scene.meshes().getPositions().size(), 11u);
            ASSERT_EQ(scene.meshes().getIndices().size(), 15u);
            ASSERT_EQ(scene.meshes().getRows()[last].mVertices.mOffset, 7u);

            const std::uint64_t was = scene.getStructureRevision();
            const std::array keep{ first, last };
            const std::array<Index, 0> noMaterials{};
            ASSERT_TRUE(scene.release(keep, noMaterials));

            // Nothing moved, nothing shrank, and every index still means what it meant.
            EXPECT_EQ(scene.meshes().getRows().size(), 3u);
            EXPECT_EQ(scene.meshes().getPositions().size(), 11u);
            EXPECT_EQ(scene.meshes().getIndices().size(), 15u);
            EXPECT_EQ(scene.meshes().getRows()[last].mVertices.mOffset, 7u);
            EXPECT_EQ(scene.meshes().getMeshPositions(first)[0].z(), 0.0f);
            EXPECT_EQ(scene.meshes().getMeshPositions(last)[0].z(), 2.0f);

            // The freed one describes nothing until something takes it, so a backend that walks the
            // table builds a structure over no triangles rather than over somebody else's.
            EXPECT_EQ(scene.meshes().getRows()[middle].mVertices.mCount, 0u);
            EXPECT_EQ(scene.meshes().getRows()[middle].mIndices.mCount, 0u);

            EXPECT_EQ(scene.getStructureRevision(), was)
                << "nothing arrived, so nothing built from these indices is out of date";

            // **The sweep names the slot it gave up, and it stops being an arrival by naming it.**
            // Nothing has been handed over, so all three are still spoken for — two as arrivals and
            // the third as a departure, never as both.
            EXPECT_EQ(sorted(scene.meshes().getFreed()), (std::vector<Index>{ middle }));
            EXPECT_EQ(sorted(scene.meshes().getArrived()), (std::vector<Index>{ first, last }));

            // A triangle fits the hole exactly and takes it back, at the index and the offset the
            // old one had.
            const Index moved
                = scene.addMesh(MeshArrays{ .mPositions = triangleAt(5.0f), .mIndices = Testing::sTriangleIndices });
            EXPECT_EQ(moved, middle);
            EXPECT_EQ(scene.meshes().getRows()[moved].mVertices.mOffset, 4u);
            EXPECT_EQ(scene.meshes().getRows()[moved].mVertices.mCount, 3u);
            EXPECT_EQ(scene.meshes().getPositions().size(), 11u) << "a reused slot appended";
            EXPECT_GT(scene.getStructureRevision(), was) << "a slot taken over holds different geometry";

            // And the last mesh is still where it was, which a compaction is what would break.
            EXPECT_EQ(scene.meshes().getMeshPositions(last)[0].z(), 2.0f);

            // **Taking the slot back moves it the other way**, which is what lets a backend apply
            // the two lists in either order: this slot is built and not then destroyed, whichever
            // half it does first.
            EXPECT_EQ(sorted(scene.meshes().getArrived()), (std::vector<Index>{ first, moved, last }));
            EXPECT_TRUE(scene.meshes().getFreed().empty()) << "a slot taken back was still reported as gone";

            scene.clearArrivals();
            EXPECT_TRUE(scene.meshes().getArrived().empty());
            EXPECT_TRUE(scene.meshes().getFreed().empty());
        }

        /// A mesh arriving is told from a texture arriving, and a reused slot counts as an arrival.
        ///
        /// **The guard a backend builds on, and getting it wrong crashes.** `VulkanRenderer` rebuilds
        /// its acceleration structures when a mesh arrives and not when a texture does, and it used
        /// to ask the table's *size* — which cannot see a freed slot taken over by something else.
        /// A skinned body landing in one was then refitted into a bottom-level structure that had
        /// never been made for it, which is a build into a null handle.
        TEST(RtxSceneDescTest, aMeshArrivingIsToldFromATextureArrivingAndAReusedSlotIsAnArrival)
        {
            SceneDesc scene;
            const Index slot
                = scene.addMesh(MeshArrays{ .mPositions = Testing::sUnitQuad, .mIndices = Testing::sQuadIndices });

            const std::uint64_t meshes = scene.meshes().getRevision();
            const std::uint64_t structure = scene.getStructureRevision();

            // A texture is an upload, not a structure to build.
            scene.textures().add(VFS::Path::NormalizedView("textures/tx_stone.dds"));
            EXPECT_EQ(scene.meshes().getRevision(), meshes) << "a texture asked for the structures to be built again";
            EXPECT_GT(scene.getStructureRevision(), structure);

            // The slot comes back and is taken over. The table is the same size it was, and what is
            // in it is not.
            ASSERT_TRUE(scene.release({}, {}));
            EXPECT_EQ(scene.meshes().getRevision(), meshes)
                << "a cell leaving asked for the structures to be built again";

            EXPECT_EQ(
                scene.addMesh(MeshArrays{ .mPositions = triangleAt(5.0f), .mIndices = Testing::sTriangleIndices }),
                slot);
            EXPECT_EQ(scene.meshes().getRows().size(), 1u)
                << "the table grew, so a size test would have caught this anyway";
            EXPECT_GT(scene.meshes().getRevision(), meshes) << "a slot taken over went unnoticed";
        }

        /// Room given back is reused, and a mesh with nowhere to fit appends rather than being
        /// refused.
        ///
        /// **Two meshes freed side by side are one hole and not two.** A twelve-vertex mesh arrived,
        /// then a four; both go, and what is left is a single run of twelve vertices at zero rather
        /// than a pair that between them can hold nothing bigger than the larger. That is what a
        /// cell boundary is — thousands of runs laid end to end, released together — and it is why
        /// the geometry buffers stop growing once a player has travelled a while.
        ///
        /// Hand-counted: 8, 4 and 4 vertices at offsets 0, 8 and 12, and 12, 6 and 6 indices at 0,
        /// 12 and 18. Keeping only the last leaves one vertex hole of twelve at zero and one index
        /// hole of eighteen at zero.
        TEST(RtxSceneDescTest, roomGivenBackIsMergedAndReused)
        {
            SceneDesc scene;

            // Eight vertices and twelve indices, which is two quads' worth in one mesh.
            std::vector<osg::Vec3f> big;
            std::vector<std::uint32_t> bigIndices;
            for (int copy = 0; copy < 2; ++copy)
            {
                for (const osg::Vec3f& vertex : quadAt(static_cast<float>(copy)))
                    big.push_back(vertex);

                for (const std::uint32_t index : Testing::sQuadIndices)
                    bigIndices.push_back(index + static_cast<std::uint32_t>(copy) * 4u);
            }

            const Index roomy = scene.addMesh(MeshArrays{ .mPositions = big, .mIndices = bigIndices });
            const Index snug
                = scene.addMesh(MeshArrays{ .mPositions = Testing::sUnitQuad, .mIndices = Testing::sQuadIndices });
            const Index kept
                = scene.addMesh(MeshArrays{ .mPositions = Testing::sUnitQuad, .mIndices = Testing::sQuadIndices });

            ASSERT_EQ(scene.meshes().getRows()[roomy].mVertices.mOffset, 0u);
            ASSERT_EQ(scene.meshes().getRows()[snug].mVertices.mOffset, 8u);
            ASSERT_EQ(scene.meshes().getRows()[kept].mVertices.mOffset, 12u);

            const std::array keep{ kept };
            ASSERT_TRUE(scene.release(keep, {}));

            // Exactly the two that went, once each. Sorted, because which way a sweep walks its
            // table is not something a backend should have to know.
            EXPECT_EQ(sorted(scene.meshes().getFreed()), (std::vector<Index>{ roomy, snug }));

            const std::size_t vertices = scene.meshes().getPositions().size();
            ASSERT_EQ(vertices, 16u);

            // The quad takes the front of the merged hole and leaves eight vertices behind it.
            const Index quad
                = scene.addMesh(MeshArrays{ .mPositions = Testing::sUnitQuad, .mIndices = Testing::sQuadIndices });
            EXPECT_EQ(scene.meshes().getRows()[quad].mVertices.mOffset, 0u);

            // **Which is what the eight-vertex mesh then fits into.** Unmerged, the two holes were
            // eight and four and the four had just been spent, so this would have appended.
            const Index again = scene.addMesh(MeshArrays{ .mPositions = big, .mIndices = bigIndices });
            EXPECT_EQ(scene.meshes().getRows()[again].mVertices.mOffset, 4u);
            EXPECT_EQ(scene.meshes().getPositions().size(), vertices) << "a mesh that fitted a hole appended anyway";

            // Both freed slots have been taken, the lower one first — `Rtx::SlotRows` says why a
            // table answers with the lowest and never with the last one given back.
            EXPECT_EQ(quad, roomy);
            EXPECT_EQ(again, snug);

            // Nothing fits now, so this one goes on the end.
            EXPECT_EQ(scene.addMesh(MeshArrays{ .mPositions = big, .mIndices = bigIndices }), 3u);
            EXPECT_GT(scene.meshes().getPositions().size(), vertices);
        }

        /// A slot taken over holds its own attributes and none of its predecessor's.
        ///
        /// **The one way a reused slot can be quietly wrong.** A mesh that brings no normals is
        /// given zeroes on a fresh slot because the buffer was grown for it; on a reused one the
        /// room already holds whatever the last tenant put there, and a surface lit by somebody
        /// else's normals looks lit rather than looking broken.
        ///
        /// **What stands for nothing is not the same in every buffer.** A zero normal says "use
        /// the triangle's plane" and a white colour says "no tint", because a hit reads the first
        /// and multiplies by the second — so a slot given a black colour would go dark rather than
        /// untinted.
        TEST(RtxSceneDescTest, aReusedSlotDoesNotInheritTheAttributesOfWhatStoodInIt)
        {
            SceneDesc scene;
            const MeshTable& meshes = scene.meshes();

            const std::array<osg::Vec3f, 4> normals{ osg::Vec3f(1.0f, 0.0f, 0.0f), osg::Vec3f(1.0f, 0.0f, 0.0f),
                osg::Vec3f(1.0f, 0.0f, 0.0f), osg::Vec3f(1.0f, 0.0f, 0.0f) };
            const std::array<osg::Vec2f, 4> uvs{ osg::Vec2f(0.5f, 0.5f), osg::Vec2f(0.5f, 0.5f), osg::Vec2f(0.5f, 0.5f),
                osg::Vec2f(0.5f, 0.5f) };
            const std::array<osg::Vec3f, 4> colours{ osg::Vec3f(0.25f, 0.0f, 0.0f), osg::Vec3f(0.25f, 0.0f, 0.0f),
                osg::Vec3f(0.25f, 0.0f, 0.0f), osg::Vec3f(0.25f, 0.0f, 0.0f) };

            const Index slot = scene.addMesh(MeshArrays{ .mPositions = Testing::sUnitQuad,
                .mNormals = normals,
                .mTexCoords = uvs,
                .mColours = colours,
                .mIndices = Testing::sQuadIndices });
            ASSERT_EQ(meshes.getNormals()[meshes.getRows()[slot].mVertices.mOffset], osg::Vec3f(1.0f, 0.0f, 0.0f));
            ASSERT_EQ(meshes.getColours()[meshes.getRows()[slot].mVertices.mOffset], osg::Vec3f(0.25f, 0.0f, 0.0f));

            ASSERT_TRUE(scene.release({}, {}));
            EXPECT_EQ(
                scene.addMesh(MeshArrays{ .mPositions = Testing::sUnitQuad, .mIndices = Testing::sQuadIndices }), slot);

            EXPECT_EQ(meshes.getNormals()[meshes.getRows()[slot].mVertices.mOffset], osg::Vec3f())
                << "the slot kept the last tenant's normals";
            EXPECT_EQ(meshes.getTexCoords()[0], osg::Vec2f());
            EXPECT_EQ(meshes.getColours()[meshes.getRows()[slot].mVertices.mOffset], osg::Vec3f(1.0f, 1.0f, 1.0f))
                << "the slot kept the last tenant's tint";
        }

        /// A material frees its slot, and the layer run and masks behind it come back too.
        ///
        /// Hand-counted: three materials, of which the first and last are terrain with one and two
        /// layers. The layers sit at 0, 1 and 2 and their masks at 0 and 4, nine weights of the
        /// second sitting behind four of the first. Freeing the first leaves a one-long hole in the
        /// layer table and a four-long one in the masks, and the next chunk of the same shape lands
        /// in both — which is the difference between travelling and accumulating a blend map per
        /// chunk walked past.
        /// Three materials over four textures, with the first released — the state both tests below
        /// are each about one part of.
        ///
        /// **A struct rather than a fixture**, because `RtxSceneDescTest` is a suite of plain tests
        /// and one shared setup does not earn converting the other fifty.
        struct ReleasedTerrain
        {
            static constexpr std::array sGroundWeights{ 0.25f, 0.25f, 0.25f, 0.25f };
            static constexpr std::array sSandWeights{ 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f };

            SceneDesc mScene;

            Index mGround = mScene.textures().add(VFS::Path::NormalizedView("textures/tx_ground.dds"));
            Index mStone = mScene.textures().add(VFS::Path::NormalizedView("textures/tx_stone.dds"));
            Index mSand = mScene.textures().add(VFS::Path::NormalizedView("textures/tx_sand.dds"));
            Index mMoss = mScene.textures().add(VFS::Path::NormalizedView("textures/tx_moss.dds"));

            Index mDropped = sNoIndex;
            Index mPlain = sNoIndex;
            Index mKept = sNoIndex;

            /// True where the scene arrived at the state the tests describe, which a caller asserts
            /// on rather than trusting.
            bool mReleased = false;

            /// What the two tables held before the release, which is what "they never shrink" is
            /// measured against.
            std::size_t mLayersBefore = 0;
            std::size_t mMasksBefore = 0;

            ReleasedTerrain()
            {
                const std::array droppedLayers{ MaterialLayer{ .mDiffuse = mGround,
                    .mMask = mScene.materials().addMask(sGroundWeights),
                    .mMaskWidth = 2,
                    .mMaskHeight = 2 } };
                const Rtx::Run droppedRun = mScene.materials().addLayers(droppedLayers);
                mDropped = mScene.materials().add(Material{ .mKind = MaterialKind::Terrain, .mLayers = droppedRun });

                mPlain = mScene.materials().add(Material{ .mDiffuse = mStone });

                const std::array keptLayers{ MaterialLayer{ .mDiffuse = mSand,
                                                 .mMask = mScene.materials().addMask(sSandWeights),
                                                 .mMaskWidth = 3,
                                                 .mMaskHeight = 3 },
                    MaterialLayer{ .mDiffuse = mMoss } };
                const Rtx::Run keptRun = mScene.materials().addLayers(keptLayers);
                mKept = mScene.materials().add(Material{ .mKind = MaterialKind::Terrain, .mLayers = keptRun });

                mLayersBefore = mScene.materials().getLayers().size();
                mMasksBefore = mScene.materials().getMasks().size();

                const std::array<Index, 0> noMeshes{};
                const std::array materials{ mPlain, mKept };
                mReleased = mScene.release(noMeshes, materials);
            }
        };

        TEST(RtxSceneDescTest, releasingAMaterialGivesBackItsLayersAndMasks)
        {
            ReleasedTerrain terrain;
            SceneDesc& scene = terrain.mScene;
            ASSERT_TRUE(terrain.mReleased);
            ASSERT_EQ(terrain.mMoss, 3u);
            ASSERT_EQ(terrain.mLayersBefore, 3u);
            ASSERT_EQ(terrain.mMasksBefore, 13u);

            // Every survivor is at the index it was given, which is what nothing moving means.
            EXPECT_EQ(scene.materials().getRows().size(), 3u);
            EXPECT_EQ(scene.materials().getRows()[terrain.mPlain].mDiffuse, terrain.mStone);
            EXPECT_EQ(scene.materials().getRows()[terrain.mKept].mLayers, (Rtx::Run{ .mOffset = 1, .mCount = 2 }));

            EXPECT_EQ(scene.materials().getLayers().size(), terrain.mLayersBefore)
                << "the tables never shrink, they are reused in place";
            EXPECT_EQ(scene.materials().getMasks().size(), terrain.mMasksBefore);
            EXPECT_EQ(scene.materials().getLayers()[1].mDiffuse, terrain.mSand);
            EXPECT_EQ(scene.materials().getLayers()[2].mDiffuse, terrain.mMoss);

            // **The next chunk of the same shape lands in the hole the first one left.** One layer
            // and four weights, which is exactly what went: both come back at zero and neither table
            // is any longer than it was.
            const std::array arrivingLayers{ MaterialLayer{ .mDiffuse = terrain.mMoss,
                .mMask = scene.materials().addMask(ReleasedTerrain::sGroundWeights),
                .mMaskWidth = 2,
                .mMaskHeight = 2 } };
            const Rtx::Run arrivingRun = scene.materials().addLayers(arrivingLayers);

            EXPECT_EQ(arrivingLayers[0].mMask, (Rtx::Run{ .mOffset = 0, .mCount = 4 })) << "the freed mask run";
            EXPECT_EQ(arrivingRun, (Rtx::Run{ .mOffset = 0, .mCount = 1 })) << "the freed layer run";
            EXPECT_EQ(scene.materials().getLayers().size(), terrain.mLayersBefore)
                << "the layer table grew past a hole that fitted";
            EXPECT_EQ(scene.materials().getMasks().size(), terrain.mMasksBefore)
                << "the mask table grew past a hole that fitted";

            // The freed slot goes to the next material asked for, whatever size it is: a material is
            // one size, so there is no fit to find.
            EXPECT_EQ(scene.materials().add(Material{ .mDiffuse = terrain.mMoss }), terrain.mDropped);
            EXPECT_EQ(scene.materials().getRows().size(), 3u);
        }

        /// **`tx_ground` goes with the layer that named it, and its slot comes back.** Only the dead
        /// material's run wore it, and an orphaned run is deliberately not allowed to speak for a
        /// texture — or the image would leak alongside the layers.
        TEST(RtxSceneDescTest, releasingAMaterialGivesBackTheTextureOnlyItWore)
        {
            ReleasedTerrain terrain;
            SceneDesc& scene = terrain.mScene;
            ASSERT_TRUE(terrain.mReleased);

            // One texture went with the material that wore it, and it stopped being an arrival.
            EXPECT_EQ(sorted(scene.textures().getFreed()), (std::vector<Index>{ terrain.mGround }));
            EXPECT_EQ(sorted(scene.textures().getArrived()),
                (std::vector<Index>{ terrain.mStone, terrain.mSand, terrain.mMoss }));

            ASSERT_EQ(scene.textures().getPaths().size(), 4u) << "the table shrank, so something was renumbered";
            EXPECT_TRUE(scene.textures().getPaths()[terrain.mGround].value().empty())
                << "a texture nothing wears was kept";

            // The three the survivors wear are untouched, at the indices they were given.
            EXPECT_EQ(scene.textures().getPaths()[terrain.mStone], VFS::Path::NormalizedView("textures/tx_stone.dds"));
            EXPECT_EQ(scene.textures().getPaths()[terrain.mSand], VFS::Path::NormalizedView("textures/tx_sand.dds"));
            EXPECT_EQ(scene.textures().getPaths()[terrain.mMoss], VFS::Path::NormalizedView("textures/tx_moss.dds"));

            // The freed slot is what the next texture takes, and the path lookup went with it: asking
            // for `tx_ground` again is a new arrival rather than a hit on a slot nothing stands in.
            EXPECT_EQ(scene.textures().add(VFS::Path::NormalizedView("textures/tx_ground.dds")), terrain.mGround);
            EXPECT_EQ(scene.textures().getPaths().size(), 4u) << "the table grew past a free slot";
            EXPECT_EQ(scene.textures().getArrived().back(), terrain.mGround)
                << "a slot taken over was not reported as arriving";
            EXPECT_TRUE(scene.textures().getFreed().empty()) << "a slot taken back was still reported as gone";
        }

        /// **The split that keeps an animated state set from rebuilding the world.**
        ///
        /// A material appearing is one row of a table, and a sweep that takes one away again is not
        /// even that. A mesh or a texture *appearing* is every acceleration structure in the scene.
        /// The mirror reports them apart so a reader can answer them apart — OpenMW's water cycles
        /// thirty-two materials a second, and reading that as a world arriving cost the game every
        /// frame it had.
        TEST(RtxSceneDescTest, aMaterialChangingIsNotAStructureChanging)
        {
            SceneDesc scene;
            const Index mesh
                = scene.addMesh(MeshArrays{ .mPositions = Testing::sUnitQuad, .mIndices = Testing::sQuadIndices });
            const Index first = scene.materials().add(Material{});

            const std::uint64_t structure = scene.getStructureRevision();
            scene.clearArrivals();

            // A second material, which is what a state set with a new address comes to.
            Material other;
            other.mTwoSided = true;
            const Index kept = scene.materials().add(other);

            EXPECT_EQ(scene.getStructureRevision(), structure) << "a material asked for a rebuild";
            EXPECT_EQ(sorted(scene.materials().getWritten()), (std::vector<Index>{ kept }))
                << "the row that arrived, and only it";

            // And taking one away again is no shading change at all: nothing stands on the row, so
            // nothing reads it and nothing has to write it.
            scene.clearArrivals();
            const std::array meshes{ mesh };
            const std::array materials{ kept };

            ASSERT_TRUE(scene.release(meshes, materials));
            EXPECT_EQ(scene.getStructureRevision(), structure) << "a sweep of one material asked for a rebuild";
            EXPECT_TRUE(scene.materials().getWritten().empty()) << "a sweep reported a row to write";

            // **And a mesh going is no longer the other answer either.** It was, while a sweep
            // compacted: the table moved and everything built from it had to be built again. A slot
            // that is freed in place invalidates nothing, so the frame after a cell leaves costs the
            // top level and nothing else.
            const std::uint64_t before = scene.getStructureRevision();
            ASSERT_TRUE(scene.release({}, materials));
            EXPECT_EQ(scene.getStructureRevision(), before) << "a cell leaving asked for a rebuild";

            // **The slot the sweep freed is taken over, and that is a row again.** A flipbook added
            // and then rewritten on one frame is one row too: the list holds each slot once.
            EXPECT_EQ(scene.materials().add(Material{}), first) << "a freed slot was not the one handed out";
            scene.setMaterial(first, other);
            EXPECT_EQ(sorted(scene.materials().getWritten()), (std::vector<Index>{ first }));

            // A rewrite that changes nothing is not a write, which is what a paused game is.
            scene.clearArrivals();
            scene.setMaterial(first, other);
            EXPECT_TRUE(scene.materials().getWritten().empty()) << "writing back what was there reported a row";
        }

        /// A chunk's layers and weights arrive as the runs they were placed in, and a chunk that
        /// leaves gives its runs back without naming them.
        ///
        /// **What lets the mask table stay where it is.** The runs are what a backend copies; a
        /// flag over the table would have it copy the whole of it, which is megabytes for a chunk
        /// that brought a few hundred floats.
        TEST(RtxSceneDescTest, layersAndMasksArriveAsTheRunsTheyWerePlacedIn)
        {
            SceneDesc scene;

            const std::array<float, 4> weights{ 1.0f, 0.0f, 0.0f, 1.0f };
            const Rtx::Run mask = scene.materials().addMask(weights);
            EXPECT_EQ(mask, (Rtx::Run{ .mOffset = 0, .mCount = 4 }));
            EXPECT_EQ(runs(scene.materials().getArrived().mMasks),
                (std::vector<Rtx::Run>{ Rtx::Run{ .mOffset = 0, .mCount = 4 } }));

            const std::array layers{
                MaterialLayer{ .mMask = mask, .mMaskWidth = 2, .mMaskHeight = 2 },
                MaterialLayer{},
            };
            const Rtx::Run run = scene.materials().addLayers(layers);
            EXPECT_EQ(run, (Rtx::Run{ .mOffset = 0, .mCount = 2 }));
            EXPECT_EQ(runs(scene.materials().getArrived().mLayers), (std::vector<Rtx::Run>{ run }));

            scene.materials().add(Material{ .mKind = MaterialKind::Terrain, .mLayers = run });
            scene.clearArrivals();
            EXPECT_TRUE(scene.materials().getArrived().mMasks.empty());
            EXPECT_TRUE(scene.materials().getArrived().mLayers.empty());

            // A second chunk lands past the first: its runs are its own and say where they are.
            const std::array<float, 2> more{ 0.5f, 0.5f };
            EXPECT_EQ(scene.materials().addMask(more), (Rtx::Run{ .mOffset = 4, .mCount = 2 }));
            EXPECT_EQ(runs(scene.materials().getArrived().mMasks),
                (std::vector<Rtx::Run>{ Rtx::Run{ .mOffset = 4, .mCount = 2 } }));

            const std::array one{ MaterialLayer{
                .mMask = Rtx::Run{ .mOffset = 4, .mCount = 2 }, .mMaskWidth = 2, .mMaskHeight = 1 } };
            EXPECT_EQ(scene.materials().addLayers(one), (Rtx::Run{ .mOffset = 2, .mCount = 1 }));
            EXPECT_EQ(runs(scene.materials().getArrived().mLayers),
                (std::vector<Rtx::Run>{ Rtx::Run{ .mOffset = 2, .mCount = 1 } }));
            scene.clearArrivals();

            // The first chunk goes and its runs go with it — reported to nobody, because nothing
            // reads a run nothing names. The next chunk that fits lands in the hole, and that
            // arrival is what names the run again.
            const std::array<Index, 0> noMeshes{};
            const std::array keep{ scene.materials().add(Material{}) };
            scene.clearArrivals();
            ASSERT_TRUE(scene.release(noMeshes, keep));
            EXPECT_TRUE(scene.materials().getArrived().mMasks.empty()) << "a sweep reported a run to write";
            EXPECT_TRUE(scene.materials().getArrived().mLayers.empty());
            EXPECT_TRUE(scene.materials().getWritten().empty());

            EXPECT_EQ(scene.materials().addMask(weights), (Rtx::Run{ .mOffset = 0, .mCount = 4 }))
                << "the freed run was not the one handed out";
            EXPECT_EQ(runs(scene.materials().getArrived().mMasks),
                (std::vector<Rtx::Run>{ Rtx::Run{ .mOffset = 0, .mCount = 4 } }));
        }

        /// A scene that lost nothing is left entirely alone, and a sprite's texture is the caller's
        /// to speak for.
        TEST(RtxSceneDescTest, releasingDoesNothingWhenNothingWent)
        {
            SceneDesc scene;
            const Index mesh
                = scene.addMesh(MeshArrays{ .mPositions = Testing::sUnitQuad, .mIndices = Testing::sQuadIndices });
            const Index material = scene.materials().add(Material{});
            scene.textures().add(VFS::Path::NormalizedView("textures/tx_fire_00.dds"));

            const std::array meshes{ mesh };
            const std::array materials{ material };
            const std::uint64_t was = scene.getStructureRevision();
            scene.clearArrivals();

            EXPECT_FALSE(scene.release(meshes, materials));
            EXPECT_EQ(scene.getStructureRevision(), was);
            EXPECT_TRUE(scene.materials().getWritten().empty());
            EXPECT_TRUE(scene.meshes().getFreed().empty()) << "a sweep that freed nothing named something";
            EXPECT_TRUE(scene.textures().getFreed().empty());

            // Asked again with everything already free, which is the frame after a cell left: the
            // live count is what the keep set is compared against, not the table's size.
            const std::array<Index, 0> none{};
            ASSERT_TRUE(scene.release(none, none));
            EXPECT_EQ(sorted(scene.meshes().getFreed()), (std::vector<Index>{ mesh }));

            EXPECT_FALSE(scene.release(none, none)) << "a table with nothing left in it went again";
            EXPECT_EQ(sorted(scene.meshes().getFreed()), (std::vector<Index>{ mesh })) << "a slot went twice";

            // A texture nothing has been told to name is nobody's to give back, so it stays — which
            // is what `addTexture` says of a caller that asks for one and then puts it nowhere.
            EXPECT_EQ(scene.textures().getPaths().size(), 1u);
            EXPECT_TRUE(scene.textures().getFreed().empty());
        }

        /// A sweep leaves the per-frame lists as the walk left them, because the frame it happens on
        /// is about to be drawn from them. Emptying them here left every lamp in the world dark for
        /// exactly one frame, on the frames a sweep freed something.
        TEST(RtxSceneDescTest, aSweepLeavesTheListsTheWalkFilled)
        {
            SceneDesc scene;
            const Index kept
                = scene.addMesh(MeshArrays{ .mPositions = Testing::sUnitQuad, .mIndices = Testing::sQuadIndices });
            scene.addMesh(MeshArrays{ .mPositions = Testing::sUnitQuad, .mIndices = Testing::sQuadIndices });

            scene.addLight(Light{ .mPosition = osg::Vec3f(1.0f, 2.0f, 3.0f),
                .mIntensity = osg::Vec3f(4.0f, 5.0f, 6.0f),
                .mReach = 256.0f });
            scene.addLight(Light{ .mPosition = osg::Vec3f(-7.0f, 8.0f, 9.0f),
                .mIntensity = osg::Vec3f(1.0f, 1.0f, 1.0f),
                .mReach = 512.0f });

            const std::array meshes{ kept };
            const std::array<Index, 0> noMaterials{};
            ASSERT_TRUE(scene.release(meshes, noMaterials)) << "the second mesh should have gone";

            ASSERT_EQ(scene.lights().size(), 2u) << "the sweep emptied the light table the walk had just filled";
            EXPECT_EQ(scene.lights()[0].mPosition, osg::Vec3f(1.0f, 2.0f, 3.0f));
            EXPECT_EQ(scene.lights()[0].mReach, 256.0f);
            EXPECT_EQ(scene.lights()[1].mPosition, osg::Vec3f(-7.0f, 8.0f, 9.0f));
            EXPECT_EQ(scene.lights()[1].mReach, 512.0f);

            // Emptying them is still `clearPlacement`'s, which is what the next walk begins with.
            scene.clearPlacement();
            EXPECT_TRUE(scene.lights().empty());
        }

        /// A texture goes with the last material that names it, and not with the first.
        ///
        /// **The case a sweep could only answer on some frames.** Freeing used to be a walk of the
        /// live materials run from `release`, and `release` returns before it starts whenever the
        /// mesh and material counts say nothing died. Counting the names instead makes the answer
        /// the same whatever else the frame did.
        TEST(RtxSceneDescTest, aTextureGoesWithTheLastMaterialThatNamesIt)
        {
            SceneDesc scene;
            const Index mesh
                = scene.addMesh(MeshArrays{ .mPositions = Testing::sUnitQuad, .mIndices = Testing::sQuadIndices });
            const Index shared = scene.textures().add(VFS::Path::NormalizedView("textures/tx_stone.dds"));
            const Index lone = scene.textures().add(VFS::Path::NormalizedView("textures/tx_sand.dds"));

            scene.materials().add(Material{ .mDiffuse = shared });
            const Index second = scene.materials().add(Material{ .mDiffuse = shared, .mNormal = lone });

            const std::array meshes{ mesh };
            const std::array keepSecond{ second };
            ASSERT_TRUE(scene.release(meshes, keepSecond));

            EXPECT_TRUE(scene.textures().getFreed().empty()) << "a texture another material still names";
            EXPECT_EQ(scene.textures().getPaths()[shared], VFS::Path::NormalizedView("textures/tx_stone.dds"));

            const std::array<Index, 0> none{};
            ASSERT_TRUE(scene.release(meshes, none));

            EXPECT_EQ(sorted(scene.textures().getFreed()), (std::vector<Index>{ shared, lone }));
            EXPECT_TRUE(scene.textures().getPaths()[shared].value().empty());
            EXPECT_TRUE(scene.textures().getPaths()[lone].value().empty());
        }

        /// An image with no file behind it takes a slot like any other and gives it back like any
        /// other.
        ///
        /// **What a composite baked for a distant terrain chunk is.** Nothing can open it — the bytes
        /// belong to whatever made it — but it is still a slot a material points at and a backend
        /// uploads into, so it has to live in the one table, on the one free list, under the one
        /// reference count. A second table would be a second lifetime for a thing that dies the same
        /// way.
        TEST(RtxSceneDescTest, anImageThatIsNotAFileTakesASlotAndGivesItBack)
        {
            SceneDesc scene;

            const Index baked = scene.textures().addBaked("composite/-3,-2/2");
            ASSERT_EQ(baked, 0u);

            // Standing, and standing is not free — the path is empty because it has none, which is
            // the same thing a free slot's path says and not the same fact.
            EXPECT_FALSE(scene.textures().isFree(baked));
            EXPECT_TRUE(scene.textures().getPaths()[baked].value().empty()) << "it came from no file";
            EXPECT_EQ(scene.textures().getBaked()[baked], "composite/-3,-2/2");

            // The key is what makes two chunks that would bake the same image share one slot.
            EXPECT_EQ(scene.textures().addBaked("composite/-3,-2/2"), baked) << "the same bake took a second slot";
            EXPECT_EQ(scene.textures().getPaths().size(), 1u);

            // A file beside it, so the free list has to hand back the right one.
            const Index file = scene.textures().add(VFS::Path::NormalizedView("textures/tx_stone.dds"));
            ASSERT_EQ(file, 1u);

            scene.textures().hold(baked);
            scene.textures().drop(baked);

            EXPECT_TRUE(scene.textures().isFree(baked)) << "nothing names it and it is still standing";
            EXPECT_TRUE(scene.textures().getBaked()[baked].empty());
            EXPECT_EQ(sorted(scene.textures().getFreed()), (std::vector<Index>{ baked }));

            // And the slot comes back, to a file this time — a freed slot is a row and not a kind.
            const Index next = scene.textures().add(VFS::Path::NormalizedView("textures/tx_sand.dds"));
            EXPECT_EQ(next, baked) << "the table grew past a free slot";
            EXPECT_EQ(scene.textures().getPaths().size(), 2u);
            EXPECT_EQ(scene.textures().getPaths()[next], VFS::Path::NormalizedView("textures/tx_sand.dds"));
            EXPECT_TRUE(scene.textures().getBaked()[next].empty()) << "the slot kept what the last tenant was";

            // The key is free again too, or a bake that came back would find a slot somebody else has.
            const Index again = scene.textures().addBaked("composite/-3,-2/2");
            EXPECT_EQ(again, 2u) << "a key the table gave back found a slot somebody else has";
            EXPECT_FALSE(scene.textures().isFree(file)) << "the file beside it was never touched";
        }

        /// A material rewritten gives back what it stopped naming and keeps what it still names.
        ///
        /// **What a flipbook is**: `NifOsg` turns a fire over thirty-two times a second by rewriting
        /// one state set, and the surface wearing it never moves. The material keeps its slot; the
        /// image it walked away from does not.
        TEST(RtxSceneDescTest, aMaterialRewrittenGivesBackOnlyWhatItStoppedNaming)
        {
            SceneDesc scene;
            const Index first = scene.textures().add(VFS::Path::NormalizedView("textures/tx_fire_00.dds"));
            const Index second = scene.textures().add(VFS::Path::NormalizedView("textures/tx_fire_01.dds"));
            const Index material = scene.materials().add(Material{ .mDiffuse = first });

            scene.setMaterial(material, Material{ .mDiffuse = second });

            EXPECT_EQ(sorted(scene.textures().getFreed()), (std::vector<Index>{ first }));
            EXPECT_TRUE(scene.textures().getPaths()[first].value().empty()) << "the frame it left is still named";
            EXPECT_EQ(scene.textures().getPaths()[second], VFS::Path::NormalizedView("textures/tx_fire_01.dds"));

            // **And round again onto a frame it already had.** Taking the new set before giving the
            // old one back is the whole of what stops this: the other order takes the slot to zero,
            // empties its path and hands it to the next thing that asks for one — a texture changing
            // identity under a material that never stopped naming it.
            scene.setMaterial(material, Material{ .mDiffuse = second, .mTwoSided = true });

            EXPECT_EQ(scene.textures().getPaths()[second], VFS::Path::NormalizedView("textures/tx_fire_01.dds"))
                << "a texture the material still names was let go and taken again";
            EXPECT_EQ(sorted(scene.textures().getFreed()), (std::vector<Index>{ first })) << "and reported as going";
        }

        /// A hold speaks for a texture no material can, and the slot goes when the hold does.
        TEST(RtxSceneDescTest, aHeldTextureGoesWhenTheHoldDoesAndNotBefore)
        {
            SceneDesc scene;
            const Index mesh
                = scene.addMesh(MeshArrays{ .mPositions = Testing::sUnitQuad, .mIndices = Testing::sQuadIndices });
            const Index material = scene.materials().add(Material{});
            const Index sprite = scene.textures().add(VFS::Path::NormalizedView("textures/tx_fire_00.dds"));
            scene.textures().hold(sprite);

            // The ordinary frame, where the sweep answers with two comparisons and returns.
            const std::array meshes{ mesh };
            const std::array materials{ material };
            EXPECT_FALSE(scene.release(meshes, materials));
            EXPECT_EQ(scene.textures().getPaths()[sprite], VFS::Path::NormalizedView("textures/tx_fire_00.dds"));

            scene.textures().drop(sprite);

            EXPECT_EQ(sorted(scene.textures().getFreed()), (std::vector<Index>{ sprite }));
            EXPECT_TRUE(scene.textures().getPaths()[sprite].value().empty());

            // And the slot is handed out again rather than the table growing.
            EXPECT_EQ(scene.textures().add(VFS::Path::NormalizedView("textures/tx_smoke.dds")), sprite);
            EXPECT_EQ(scene.textures().getPaths().size(), 1u);
        }

        /// A mesh's vertices never straddle a block, and the tail one skipped is handed out again.
        ///
        /// **What lets the device hold a list of buffers rather than one.** A buffer that is a single
        /// allocation moves when it grows, and every bottom-level acceleration structure holds a
        /// device address into it; blocked, each block is allocated once and never moves. The rule
        /// that buys that is the one asserted here — a run lies inside one block or it is not placed
        /// there — and the price is the tail, which must go back into circulation or a scene would
        /// leak most of a block per boundary crossed.
        ///
        /// Hand-computed against a block of 262,144. Two hundred thousand vertices leave 62,144 of
        /// the first block; a hundred thousand cannot fit in that, so it starts the second and the
        /// tail stays behind; sixty thousand then fits the tail and takes it at 200,000.
        TEST(RtxSceneDescTest, aMeshNeverStraddlesABlockAndTheTailItSkippedIsReused)
        {
            ASSERT_EQ(SceneDesc::sVertexBlock, 262144u) << "the arithmetic below is written against this";

            // One buffer, sliced. A block is a quarter of a million vertices and three separate
            // copies of that is memory this test has no use for.
            const std::vector<osg::Vec3f> room(SceneDesc::sVertexBlock);
            const std::array<std::uint32_t, 3> triangle{ 0, 1, 2 };

            const auto vertices = [&](std::size_t count) { return std::span(room).first(count); };

            SceneDesc scene;
            const Index first = scene.addMesh(MeshArrays{ .mPositions = vertices(200000), .mIndices = triangle });
            EXPECT_EQ(scene.meshes().getRows()[first].mVertices.mOffset, 0u);

            const Index second = scene.addMesh(MeshArrays{ .mPositions = vertices(100000), .mIndices = triangle });
            EXPECT_EQ(scene.meshes().getRows()[second].mVertices.mOffset, SceneDesc::sVertexBlock)
                << "a run was laid across a block boundary";
            EXPECT_EQ(scene.meshes().getPositions().size(), std::size_t{ 362144 });

            // And the 62,144 the second one stepped over is a hole like any other.
            const Index third = scene.addMesh(MeshArrays{ .mPositions = vertices(60000), .mIndices = triangle });
            EXPECT_EQ(scene.meshes().getRows()[third].mVertices.mOffset, 200000u)
                << "the tail of a block was not reused";
            EXPECT_EQ(scene.meshes().getPositions().size(), std::size_t{ 362144 })
                << "a mesh that fitted the tail appended";

            // None of the three crosses a boundary, which is the property rather than the three
            // offsets that happen to demonstrate it.
            for (const Index mesh : { first, second, third })
            {
                const MeshRange& range = scene.meshes().getRows()[mesh];
                EXPECT_EQ(range.mVertices.mOffset / SceneDesc::sVertexBlock,
                    (range.mVertices.mOffset + range.mVertices.mCount - 1) / SceneDesc::sVertexBlock)
                    << "mesh " << mesh << " straddles a block";
            }
        }

        /// A mesh longer than a block is refused by name rather than written across two of them.
        ///
        /// **Not an assert, because a vertex count comes out of a content file.** A run that
        /// straddled a block would be written across two device allocations that are not next to
        /// each other, which is not a wrong picture but a wild write.
        TEST(RtxSceneDescTest, aMeshLongerThanABlockIsRefusedByName)
        {
            const std::vector<osg::Vec3f> tooMany(SceneDesc::sVertexBlock + 1);
            const std::array<std::uint32_t, 3> triangle{ 0, 1, 2 };

            SceneDesc scene;
            EXPECT_THROW(scene.addMesh(MeshArrays{ .mPositions = tooMany, .mIndices = triangle }), Error);

            // And exactly a block is not too many, so the refusal is a boundary and not a ban.
            EXPECT_NO_THROW(scene.addMesh(
                MeshArrays{ .mPositions = std::span(tooMany).first(SceneDesc::sVertexBlock), .mIndices = triangle }));
        }

        /// A camera is placed from what stands in a region, and the sea is not among it.
        ///
        /// **Two failures, one call.** The sea is one sheet a hundred and fifty cells across, laid
        /// down by the world rather than by any cell, so framing everything placed put the eye a
        /// million and a half units from a village. And the ground now reaches four cells past the
        /// one being looked at, so framing everything that is not the sea still framed a region. A
        /// view names a place; this is the extent of that place.
        TEST(RtxSceneDescTest, aRegionsExtentLeavesOutTheSeaAndStopsAtItsOwnEdge)
        {
            SceneDesc scene;

            const Index quad
                = scene.addMesh(MeshArrays{ .mPositions = Testing::sUnitQuad, .mIndices = Testing::sQuadIndices });
            const Index ground = scene.materials().add(Material{ .mKind = MaterialKind::Terrain });
            const Index sea = scene.materials().add(Material{ .mKind = MaterialKind::Water });

            // One unit square at the origin, and a sheet ten thousand across under everything.
            scene.addInstance(
                MeshInstance{ .mTransform = osg::Matrixf::identity(), .mMesh = quad, .mMaterial = ground });
            scene.addInstance(MeshInstance{
                .mTransform = osg::Matrixf::scale(10000.0f, 10000.0f, 1.0f), .mMesh = quad, .mMaterial = sea });

            // Everything, which is what a far plane asks for and why the sea is still in the table.
            EXPECT_FLOAT_EQ(scene.getBounds().xMax(), 10000.0f);

            const osg::BoundingBoxf everywhere(-1e9f, -1e9f, -1e9f, 1e9f, 1e9f, 1e9f);
            const osg::BoundingBoxf content = scene.getContentBoundsWithin(everywhere);

            ASSERT_TRUE(content.valid());
            EXPECT_FLOAT_EQ(content.xMax(), 1.0f) << "the sea was framed";
            EXPECT_FLOAT_EQ(content.yMax(), 1.0f);

            // **And the region clips.** A chunk straddling the edge contributes where it overlaps
            // rather than dragging the answer out by its whole width, which is what keeps a view of
            // one cell from framing the four cells of ground that reach into it.
            const Index wide
                = scene.addMesh(MeshArrays{ .mPositions = Testing::sUnitQuad, .mIndices = Testing::sQuadIndices });
            scene.addInstance(MeshInstance{
                .mTransform = osg::Matrixf::scale(100.0f, 1.0f, 1.0f), .mMesh = wide, .mMaterial = ground });

            const osg::BoundingBoxf narrow(-1.0f, -1.0f, -1.0f, 4.0f, 4.0f, 4.0f);
            const osg::BoundingBoxf clipped = scene.getContentBoundsWithin(narrow);

            ASSERT_TRUE(clipped.valid());
            EXPECT_FLOAT_EQ(clipped.xMax(), 4.0f) << "a chunk reaching past the edge widened the region";

            // Nothing stands out there, and an empty answer is what says so rather than a box at the
            // origin that a camera would then be placed from.
            EXPECT_FALSE(scene.getContentBoundsWithin(osg::BoundingBoxf(500.0f, 500.0f, 500.0f, 600.0f, 600.0f, 600.0f))
                             .valid());
        }

        /// A mesh's extent follows whatever was written into it, by either writer.
        ///
        /// **The box is kept where the positions are, and not measured where it is asked for** — so
        /// both writers owe it an answer. A skinned body reaches somewhere else on every frame it is
        /// posed, and its vertices are on the device, so the reach comes in with the pose; a slot
        /// that was given back reaches nowhere at all.
        TEST(RtxSceneDescTest, aMeshesExtentFollowsWhateverWasWrittenIntoIt)
        {
            SceneDesc scene;

            const Index quad
                = scene.addMesh(MeshArrays{ .mPositions = Testing::sUnitQuad, .mIndices = Testing::sQuadIndices }, {},
                    Deform::Rig, Testing::addOneBoneRig(scene, 4));
            const Index material = scene.materials().add(Material{});
            scene.addInstance(
                MeshInstance{ .mTransform = osg::Matrixf::identity(), .mMesh = quad, .mMaterial = material });

            // The unit square in the xy plane that the fixture is.
            EXPECT_FLOAT_EQ(scene.getBounds().xMin(), 0.0f);
            EXPECT_FLOAT_EQ(scene.getBounds().xMax(), 1.0f);

            // The same square three units along x, which is what a pose is: the count a deforming
            // mesh keeps and the places it keeps none of, with the reach the caller read.
            const std::array along{ toGpuBone(osg::Matrixf::translate(3.0f, 0.0f, 0.0f)) };
            scene.poseRig(quad, along, osg::BoundingBoxf(osg::Vec3f(3.0f, 0.0f, 0.0f), osg::Vec3f(4.0f, 1.0f, 0.0f)));

            EXPECT_FLOAT_EQ(scene.getBounds().xMin(), 3.0f) << "the extent stayed where the first pose put it";
            EXPECT_FLOAT_EQ(scene.getBounds().xMax(), 4.0f);

            // And a slot handed back reaches nowhere, however the instance standing on it is left:
            // an empty answer is what a camera is not placed from.
            ASSERT_TRUE(scene.release({}, {}));
            EXPECT_FALSE(scene.getBounds().valid());
        }

    }
}