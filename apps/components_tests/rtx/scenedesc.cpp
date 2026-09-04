#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

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
        std::vector<Span> runs(std::span<const Span> spans)
        {
            return std::vector<Span>(spans.begin(), spans.end());
        }

        /// What a news list names, sorted, so a set can be compared without depending on the order
        /// the sweep happened to walk its table in.
        std::vector<Index> sorted(std::span<const Index> slots)
        {
            std::vector<Index> copy(slots.begin(), slots.end());
            std::sort(copy.begin(), copy.end());
            return copy;
        }

        TEST(RtxSceneDescTest, aMeshRemembersWhereItsVerticesWent)
        {
            SceneDesc scene;

            const Index first = scene.addMesh(Testing::sUnitQuad, {}, {}, Testing::sQuadIndices);
            const Index second = scene.addMesh(Testing::sUnitQuad, {}, {}, Testing::sQuadIndices);

            EXPECT_EQ(first, 0u);
            EXPECT_EQ(second, 1u);

            // Two quads: 8 vertices and 12 indices in the shared buffers, the second mesh starting
            // where the first left off.
            EXPECT_EQ(scene.getPositions().size(), 8u);
            EXPECT_EQ(scene.getIndices().size(), 12u);
            EXPECT_EQ(scene.getMeshes()[1].mVertexOffset, 4u);
            EXPECT_EQ(scene.getMeshes()[1].mIndexOffset, 6u);

            EXPECT_EQ(scene.getMeshPositions(second)[2], osg::Vec3f(1.0f, 1.0f, 0.0f));
            EXPECT_EQ(scene.getMeshIndices(second)[5], 3u);

            // And whether the caller found it doubled for its back, which the scene keeps and
            // never works out for itself.
            EXPECT_FALSE(scene.getMeshes()[first].mShape.mSheet);

            // Added first and read after: the table grows under a span taken in the same expression.
            const Index sheet
                = scene.addMesh(Testing::sUnitQuad, {}, {}, Testing::sQuadIndices, FoldedShape{ .mSheet = true });
            EXPECT_TRUE(scene.getMeshes()[sheet].mShape.mSheet);
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

            scene.addMesh(Testing::sUnitQuad, {}, {}, Testing::sQuadIndices);
            const Index withNormals = scene.addMesh(Testing::sUnitQuad, sNormals, {}, Testing::sQuadIndices);

            ASSERT_EQ(scene.getNormals().size(), scene.getPositions().size());
            ASSERT_EQ(scene.getTexCoords().size(), scene.getPositions().size());

            const MeshRange& range = scene.getMeshes()[withNormals];
            EXPECT_EQ(scene.getNormals()[range.mVertexOffset], osg::Vec3f(0.0f, 0.0f, 1.0f));
            EXPECT_EQ(scene.getNormals()[0], osg::Vec3f(0.0f, 0.0f, 0.0f));
        }

        TEST(RtxSceneDescTest, aTextureIsAddedOnceHoweverOftenItIsAskedFor)
        {
            SceneDesc scene;

            constexpr VFS::Path::NormalizedView stone("textures/tx_stone_01.dds");
            constexpr VFS::Path::NormalizedView wood("textures/tx_wood_01.dds");

            EXPECT_EQ(scene.addTexture(stone), 0u);
            EXPECT_EQ(scene.addTexture(wood), 1u);
            EXPECT_EQ(scene.addTexture(stone), 0u);
            EXPECT_EQ(scene.getTextures().size(), 2u);
        }

        TEST(RtxSceneDescTest, theCountsAreWhatTheBuffersHold)
        {
            SceneDesc scene;
            scene.addMesh(Testing::sUnitQuad, {}, {}, Testing::sQuadIndices);
            scene.addMesh(Testing::sUnitQuad, {}, {}, Testing::sQuadIndices);

            EXPECT_EQ(scene.getTriangleCount(), 4u);
            EXPECT_EQ(scene.getMeshes()[0].getTriangleCount(), 2u);

            // 8 positions and 8 normals at 12 bytes, 8 texture coordinates at 8, 12 indices at 4.
            EXPECT_EQ(scene.getGeometryBytes(), 8u * 12u + 8u * 12u + 8u * 8u + 12u * 4u);
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

            const Material tested{ .mDiffuse = texture, .mAlphaRef = 0.3f, .mAlphaMode = AlphaMode::Cutout };
            EXPECT_EQ(tested.getAlphaCutoff(), 0.3f);
            EXPECT_TRUE(tested.isCutout());

            const Material blended{ .mDiffuse = texture, .mAlphaMode = AlphaMode::Blend };
            EXPECT_EQ(blended.getAlphaCutoff(), 0.5f);
            EXPECT_TRUE(blended.isCutout());

            const Material blendedWithRef{ .mDiffuse = texture, .mAlphaRef = 0.8f, .mAlphaMode = AlphaMode::Blend };
            EXPECT_EQ(blendedWithRef.getAlphaCutoff(), 0.8f);

            // The mask lives in the diffuse map's alpha, so a cutoff with no map to read it from is
            // not a cutout — and marking it one would cost traversal a candidate loop that could
            // only ever say yes.
            const Material untextured{ .mAlphaMode = AlphaMode::Blend };
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

            const Material leaf{ .mDiffuse = texture, .mAlphaMode = AlphaMode::Blend };
            EXPECT_FALSE(leaf.isTranslucent()) << "a painted mask on an opaque material";
            EXPECT_TRUE(leaf.isCutout()) << "and it keeps the branch it has";

            const Material pane{ .mDiffuse = texture,
                .mDiffuseColour = osg::Vec4f(1.0f, 1.0f, 1.0f, 0.3f),
                .mAlphaMode = AlphaMode::Blend };
            EXPECT_TRUE(pane.isTranslucent());

            // The mode is half of it: a faded material the content never asked to blend is drawn as
            // it was authored, and a cutout stays a cutout however faint its own colour is.
            const Material faded{ .mDiffuse = texture, .mDiffuseColour = osg::Vec4f(1.0f, 1.0f, 1.0f, 0.3f) };
            EXPECT_FALSE(faded.isTranslucent()) << "opaque mode, whatever the colour says";

            const Material tested{ .mDiffuse = texture,
                .mDiffuseColour = osg::Vec4f(1.0f, 1.0f, 1.0f, 0.3f),
                .mAlphaRef = 0.3f,
                .mAlphaMode = AlphaMode::Cutout };
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
            const osg::Vec4f faint(1.0f, 1.0f, 1.0f, 0.3f);

            const Material cloud{
                .mDiffuse = texture, .mDiffuseColour = faint, .mAlphaMode = AlphaMode::Blend, .mDiffuseNeverSolid = true
            };
            EXPECT_TRUE(cloud.isMedium());
            EXPECT_TRUE(cloud.getTraversed().mMedium) << "and the placements wearing it are told";

            const Material stained{ .mDiffuse = texture, .mDiffuseColour = faint, .mAlphaMode = AlphaMode::Blend };
            EXPECT_FALSE(stained.isMedium()) << "paint that closes is something to stop on";

            const Material leaf{ .mDiffuse = texture, .mAlphaMode = AlphaMode::Blend, .mDiffuseNeverSolid = true };
            EXPECT_FALSE(leaf.isMedium()) << "an opaque material, whatever its paint does";

            const Material glass{ .mDiffuseColour = faint, .mAlphaMode = AlphaMode::Blend };
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
                return mScene.addMesh(Testing::sUnitQuad, upward(), {}, Testing::sQuadIndices, {}, deform, deformer);
            }
        };

        /// A rig and the meshes on it arrive with the tables they name, and the still one is in none
        /// of them.
        TEST_F(RtxSkinnedMeshTest, aRigAndTheMeshesOnItArriveWithTheTablesTheyName)
        {
            EXPECT_TRUE(mScene.getDeformed().empty()) << "nothing has been posed yet";

            // The rig's tables: four run words and one influence, and one bone per mesh on it.
            ASSERT_EQ(mScene.getRigs().size(), 1u);
            EXPECT_EQ(mScene.getRigs()[mRig].mVertexCount, 4u);
            EXPECT_EQ(mScene.getRigs()[mRig].mBoneCount, 1u);
            EXPECT_EQ(mScene.getRigs()[mRig].mUses, 2u);
            EXPECT_EQ(mScene.getRuns().size(), 4u);
            EXPECT_EQ(mScene.getInfluences().size(), 1u);
            EXPECT_EQ(mScene.getArrivedRigs().size(), 1u);

            // The still mesh has no bind run and no rows; the two skinned ones have one apiece,
            // laid end to end.
            EXPECT_EQ(mScene.getMeshes()[mStill].mDeform, Deform::None);
            EXPECT_EQ(mScene.getMeshes()[mStill].mDeformer, sNoIndex);
            EXPECT_EQ(mScene.getMeshes()[mMoving].mDeform, Deform::Rig);
            EXPECT_EQ(mScene.getMeshes()[mMoving].mDeformer, mRig);
            EXPECT_EQ(mScene.getMeshes()[mMoving].mBindOffset, 0u);
            EXPECT_EQ(mScene.getMeshes()[mOther].mBindOffset, 4u);
            EXPECT_EQ(mScene.getBindVertexCount(), 8u) << "the bind table holds the skinned meshes alone";
            EXPECT_EQ(mScene.getMeshes()[mMoving].mPoseOffset, 0u);
            EXPECT_EQ(mScene.getMeshes()[mOther].mPoseOffset, 1u);
            EXPECT_EQ(mScene.getBones().size(), 2u);
        }

        /// A pose is rows and never vertices, and it names its mesh once a frame it moves.
        TEST_F(RtxSkinnedMeshTest, aPoseNamesItsMeshOncePerFrameAndLeavesEveryVertexAlone)
        {
            // **The first pose names the mesh whatever it is**, and a second in the same frame is
            // the same structure to refit.
            mScene.poseRig(mMoving, mAtFive, mReach);
            mScene.poseRig(mMoving, mAtFive, mReach);

            ASSERT_EQ(mScene.getDeformed().size(), 1u) << "twice in a frame is one structure to refit";
            EXPECT_EQ(mScene.getDeformed()[0], mMoving);
            EXPECT_EQ(mScene.getMeshBones(mMoving)[0].mRows[2], osg::Vec4f(0.0f, 0.0f, 1.0f, 5.0f));
            EXPECT_EQ(mScene.getMeshes()[mMoving].mBounds, mReach) << "the reach is the caller's and not the bind's";

            // The bind pose stays where it arrived, and so does everything beside it.
            EXPECT_EQ(mScene.getPositions().size(), 12u);
            EXPECT_EQ(mScene.getMeshes()[mMoving].mVertexOffset, 4u);
            EXPECT_EQ(mScene.getMeshPositions(mMoving)[2], osg::Vec3f(1.0f, 1.0f, 0.0f));
            EXPECT_EQ(mScene.getMeshPositions(mStill)[2], osg::Vec3f(1.0f, 1.0f, 0.0f));
            EXPECT_EQ(mScene.getMeshBones(mOther)[0], Shaders::GpuBone{}) << "the neighbour's rows are untouched";

            // The list is a frame's worth, so it goes when the frame's placements do.
            mScene.clearPlacement();
            EXPECT_TRUE(mScene.getDeformed().empty());
            EXPECT_EQ(mScene.getMeshes().size(), 3u) << "clearing where things are keeps what they are";

            // **A pose that did not change names nothing.** The walk poses every rig it meets and
            // cannot tell which of them the engine animated; the scene can, by looking.
            mScene.poseRig(mMoving, mAtFive, mReach);
            EXPECT_TRUE(mScene.getDeformed().empty()) << "an unchanged pose named a structure to refit";

            mScene.poseRig(mMoving, mAtSeven, mReach);
            mScene.poseRig(mOther, mAtFive, mReach);
            EXPECT_EQ(sorted(mScene.getDeformed()), (std::vector<Index>{ mMoving, mOther }));
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
            EXPECT_EQ(mScene.getRigs()[mRig].mUses, 1u);
            EXPECT_EQ(mScene.getRigs()[mRig].mVertexCount, 4u) << "a rig with a mesh on it stays";
            EXPECT_EQ(std::vector<Index>(mScene.getDeformed().begin(), mScene.getDeformed().end()),
                (std::vector<Index>{ mOther }))
                << "the freed slot left the list and the survivor stayed where it was named";

            const std::array keepOne{ mStill };
            ASSERT_TRUE(mScene.release(keepOne, {}));
            EXPECT_EQ(mScene.getRigs()[mRig].mUses, 0u);
            EXPECT_EQ(mScene.getRigs()[mRig].mVertexCount, 0u) << "a rig nothing stands on is free";
            EXPECT_TRUE(mScene.getArrivedRigs().empty());
            EXPECT_TRUE(mScene.getDeformed().empty()) << "a slot given back still named a structure to refit";

            EXPECT_EQ(Testing::addOneBoneRig(mScene, 4), mRig) << "the freed slot is the one handed out";
            EXPECT_EQ(mScene.getRuns().size(), 4u) << "the freed run is the one handed out";
            EXPECT_EQ(sorted(mScene.getArrivedRigs()), (std::vector<Index>{ mRig }));

            const Index back = addSkin();
            EXPECT_EQ(back, mOther) << "the freed mesh slot is the one handed out";
            EXPECT_EQ(mScene.getMeshes()[back].mBindOffset, 0u) << "the freed bind run is the one handed out";
            EXPECT_EQ(mScene.getBindVertexCount(), 4u)
                << "both runs went, so the table reaches only as far as this one";
            EXPECT_EQ(mScene.getMeshBones(back)[0], Shaders::GpuBone{}) << "a reused pose run holds no old pose";

            mScene.poseRig(back, mAtFive, mReach);
            EXPECT_EQ(sorted(mScene.getDeformed()), (std::vector<Index>{ back }))
                << "a reused slot's first pose names it";
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

            const Index morph = scene.addMorph(offsets, 2);
            ASSERT_EQ(scene.getMorphs().size(), 1u);
            EXPECT_EQ(scene.getMorphs()[morph].mTargetCount, 2u);
            EXPECT_EQ(scene.getMorphs()[morph].mVertexCount, 4u);
            EXPECT_EQ(scene.getMorphOffsets().size(), 8u);
            EXPECT_EQ(scene.getMorphOffsets()[6], osg::Vec3f(0.0f, 0.0f, 1.0f));
            EXPECT_EQ(sorted(scene.getArrivedMorphs()), (std::vector<Index>{ morph }));

            const Index face
                = scene.addMesh(Testing::sUnitQuad, {}, {}, Testing::sQuadIndices, {}, Deform::Morph, morph);
            EXPECT_EQ(scene.getMeshes()[face].mDeform, Deform::Morph);
            EXPECT_EQ(scene.getMorphs()[morph].mUses, 1u);
            EXPECT_EQ(scene.getWeights().size(), 2u);
            EXPECT_EQ(scene.getBindVertexCount(), 4u);

            const std::array smiling{ 1.0f, 0.5f };
            const osg::BoundingBoxf reach(osg::Vec3f(0.0f, 0.0f, 0.0f), osg::Vec3f(1.0f, 1.0f, 0.5f));
            scene.poseMorph(face, smiling, reach);
            scene.poseMorph(face, smiling, reach);
            EXPECT_EQ(sorted(scene.getDeformed()), (std::vector<Index>{ face }));
            EXPECT_EQ(scene.getMeshWeights(face)[1], 0.5f);
            EXPECT_EQ(scene.getMeshes()[face].mBounds, reach);

            scene.clearPlacement();
            scene.poseMorph(face, smiling, reach);
            EXPECT_TRUE(scene.getDeformed().empty()) << "an unchanged pose named a structure to refit";

            // The morph goes with its mesh and its offsets with it: the next set of the same shape
            // lands where they were.
            ASSERT_TRUE(scene.release({}, {}));
            EXPECT_EQ(scene.getMorphs()[morph].mUses, 0u);
            EXPECT_EQ(scene.getMorphs()[morph].mVertexCount, 0u);
            EXPECT_EQ(scene.addMorph(offsets, 2), morph);
            EXPECT_EQ(scene.getMorphOffsets().size(), 8u);
        }

        /// The finding the caller made about a mesh is kept beside its range, for a backend that
        /// builds a deforming mesh's structure to be refitted.
        TEST(RtxSceneDescTest, aMeshCarriesWhetherItDeformsAndWhatItArrivedWearing)
        {
            SceneDesc scene;
            const Index still = scene.addMesh(Testing::sUnitQuad, {}, {}, Testing::sQuadIndices);
            EXPECT_EQ(scene.getMeshes()[still].mDeform, Deform::None);
            EXPECT_EQ(scene.getMeshes()[still].mMaterial, sNoIndex);

            const Index rig = scene.addMesh(
                Testing::sUnitQuad, {}, {}, Testing::sQuadIndices, {}, Deform::Rig, Testing::addOneBoneRig(scene, 4));
            EXPECT_EQ(scene.getMeshes()[rig].mDeform, Deform::Rig);

            // The material a mesh arrives wearing is kept as it was handed over, and a slot given
            // back forgets it with the rest of what stood there.
            const Index worn = scene.addMaterial(Material{});
            const Index dressed
                = scene.addMesh(Testing::sUnitQuad, {}, {}, Testing::sQuadIndices, {}, Deform::None, sNoIndex, worn);
            EXPECT_EQ(scene.getMeshes()[dressed].mMaterial, worn);

            const std::array<Index, 2> keptMeshes{ still, rig };
            const std::array<Index, 1> keptMaterials{ worn };
            ASSERT_TRUE(scene.release(keptMeshes, keptMaterials));
            EXPECT_EQ(scene.getMeshes()[dressed].mMaterial, sNoIndex);
            EXPECT_EQ(scene.getMeshes()[dressed].mVertexCount, 0u);
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
            const Index mesh = scene.addMesh(Testing::sUnitQuad, {}, {}, Testing::sQuadIndices);
            const Index glass = scene.addMaterial(Material{
                .mDiffuseColour = osg::Vec4f(1.0f, 1.0f, 1.0f, 0.5f),
                .mAlphaMode = AlphaMode::Blend,
            });

            const Index one = scene.addInstance(MeshInstance{ .mMesh = mesh, .mMaterial = glass });
            const Index two = scene.addInstance(MeshInstance{ .mMesh = mesh, .mMaterial = glass });
            EXPECT_EQ(sorted(scene.getMoved()), (std::vector<Index>{ one, two })) << "a placement made is a row";
            EXPECT_TRUE(scene.getSettled().empty());

            scene.advancePlacement();
            EXPECT_TRUE(scene.getMoved().empty());
            EXPECT_EQ(sorted(scene.getSettled()), (std::vector<Index>{ one, two })) << "what moved is what settles";

            // A fade that changes the number is a row; one that does not is nothing. And the settled
            // list is the last moved list and nothing older.
            scene.fadeInstance(one, 0.5f);
            scene.fadeInstance(one, 0.5f);
            EXPECT_EQ(sorted(scene.getMoved()), (std::vector<Index>{ one }));
            scene.advancePlacement();
            EXPECT_EQ(sorted(scene.getSettled()), (std::vector<Index>{ one }));

            // A material crossing opaque re-classes every placement wearing it; a texture scrolling
            // under it re-classes none.
            Material worn = scene.getMaterials()[glass];
            worn.mTextureTransform = osg::Vec4f(1.0f, 1.0f, 0.25f, 0.0f);
            scene.setMaterial(glass, worn);
            EXPECT_TRUE(scene.getMoved().empty()) << "a texture scrolling reported the placements wearing it";

            worn.mDiffuseColour.a() = 1.0f;
            scene.setMaterial(glass, worn);
            EXPECT_EQ(sorted(scene.getMoved()), (std::vector<Index>{ one, two }));
            scene.advancePlacement();

            // A move is a row and a fade in the same frame is the same row twice, which is one row
            // written twice and not a wrong one.
            scene.moveInstance(two, osg::Matrixf::translate(0.0f, 0.0f, 5.0f));
            scene.fadeInstance(two, 0.25f);
            EXPECT_EQ(sorted(scene.getMoved()), (std::vector<Index>{ two, two }));
            scene.advancePlacement();

            // A dropped slot is a row to write inactive, and the slot it frees is the next
            // placement's — both reported, on the frames they happen.
            scene.dropInstance(two);
            EXPECT_EQ(sorted(scene.getMoved()), (std::vector<Index>{ two }));
            scene.advancePlacement();
            EXPECT_EQ(scene.addInstance(MeshInstance{ .mMesh = mesh }), two);
            EXPECT_EQ(sorted(scene.getMoved()), (std::vector<Index>{ two }));

            // Both lists go with the scene.
            scene.clear();
            EXPECT_TRUE(scene.getMoved().empty());
            EXPECT_TRUE(scene.getSettled().empty());
        }

        /// An emitter's sphere is derived from the sprites rather than passed in, so the rejection
        /// test a ray makes and the sprites it would then walk cannot disagree about where they are.
        ///
        /// **Off the box and not off the mean**, which the lopsided arrangement here is chosen to
        /// prove: two sprites sit at the origin and one at four along x, so the mean is at 4/3 and
        /// the box's centre at 2. From the box the reach is 2 + 1 = 3 either way; from the mean it
        /// would have to be 8/3 + 1 = 3.67 to hold the far one, a sphere 22% wider for the same
        /// three particles.
        TEST(RtxSceneDescTest, anEmitterCarriesItsSpritesAndTheSphereThatHoldsThem)
        {
            SceneDesc scene;
            const Index texture = scene.addTexture(VFS::Path::NormalizedView("textures/tx_fire_00.dds"));

            // The bake of the texture's alpha sits in the same table, which is why the count of
            // textures at the end is two.
            const Index lighting
                = scene.addBakedTexture(SpriteLightMap::keyFor(VFS::Path::NormalizedView("textures/tx_fire_00.dds")));

            const std::array sPlume{
                Sprite{ .mPosition = osg::Vec3f(0.0f, 0.0f, 0.0f), .mRadius = 1.0f },
                Sprite{ .mPosition = osg::Vec3f(0.0f, 0.0f, 0.0f), .mRadius = 1.0f },
                Sprite{ .mPosition = osg::Vec3f(4.0f, 0.0f, 0.0f), .mRadius = 1.0f },
            };
            scene.addEmitter(sPlume, texture, true, osg::Vec3f(), osg::Vec3f(), lighting);

            ASSERT_EQ(scene.getEmitters().size(), 1u);
            const SpriteEmitter& plume = scene.getEmitters().front();
            EXPECT_EQ(plume.mCentre, osg::Vec3f(2.0f, 0.0f, 0.0f));
            EXPECT_FLOAT_EQ(plume.mReach, 3.0f);
            EXPECT_EQ(plume.mFirst, 0u);
            EXPECT_EQ(plume.mCount, 3u);
            EXPECT_EQ(plume.mTexture, texture);
            EXPECT_EQ(plume.mLighting, lighting);
            EXPECT_TRUE(plume.mAdditive);

            // An emitter with nothing alive in it is not an emitter, and the next one that has
            // something starts where the first left off rather than where a placeholder would have.
            scene.addEmitter({}, texture, false);
            EXPECT_EQ(scene.getEmitters().size(), 1u);

            const std::array sSmoke{ Sprite{ .mPosition = osg::Vec3f(0.0f, 0.0f, 10.0f), .mRadius = 2.0f } };
            scene.addEmitter(sSmoke, texture, false);

            ASSERT_EQ(scene.getEmitters().size(), 2u);
            EXPECT_EQ(scene.getEmitters()[1].mFirst, 3u);
            EXPECT_EQ(scene.getEmitters()[1].mCount, 1u);
            EXPECT_FALSE(scene.getEmitters()[1].mAdditive)
                << "the blend the file asked for is what tells the two apart";
            EXPECT_EQ(scene.getEmitters()[1].mLighting, sNoIndex) << "an emitter with no bake is lit as a card";
            EXPECT_EQ(scene.getSprites().size(), 4u);
            EXPECT_EQ(scene.getSprites()[3].mPosition, osg::Vec3f(0.0f, 0.0f, 10.0f));

            // A frame's worth, so they go when the frame's placements do — and the texture they name
            // stays, because the array it indexes was uploaded when the scene was built.
            scene.clearPlacement();
            EXPECT_TRUE(scene.getEmitters().empty());
            EXPECT_TRUE(scene.getSprites().empty());
            EXPECT_EQ(scene.getTextures().size(), 2u);
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
            const Index texture = scene.addTexture(VFS::Path::NormalizedView("textures/tx_raindrop_01.dds"));

            const std::array one{ Sprite{ .mPosition = osg::Vec3f(), .mRadius = 10.0f } };

            // Facing the eye: a disc, and the reach is the radius.
            scene.addEmitter(one, texture, false);
            ASSERT_EQ(scene.getEmitters().size(), 1u);
            EXPECT_FALSE(scene.getEmitters()[0].isFixed()) << "two zero axes is a billboard";
            EXPECT_FLOAT_EQ(scene.getEmitters()[0].mReach, 10.0f);

            // Morrowind's own rain axes. The quad runs `+-0.1 * 10` across and `+-1 * 10` down, so
            // its corner is `|(0.1, 0, -1)| * 10 = 10.0499` from the middle — and that, not the ten,
            // is what has to fit in the sphere.
            const osg::Vec3f across(0.1f, 0.0f, 0.0f);
            const osg::Vec3f upward(0.0f, 0.0f, -1.0f);
            scene.addEmitter(one, texture, false, across, upward);

            ASSERT_EQ(scene.getEmitters().size(), 2u);
            const SpriteEmitter& rain = scene.getEmitters()[1];
            EXPECT_TRUE(rain.isFixed());
            EXPECT_EQ(rain.mAcross, across) << "carried as authored, because the length is the shape";
            EXPECT_EQ(rain.mUpward, upward);
            EXPECT_NEAR(rain.mReach, 10.0499f, 1e-3f);
            EXPECT_GT(rain.mReach, 10.0f) << "further than the radius alone would have reached";

            // **And an axis of nothing is not an orientation.** A system that named only one of them
            // is a billboard, which is what the zero state has to mean for a default to be safe.
            scene.addEmitter(one, texture, false, across, osg::Vec3f());
            ASSERT_EQ(scene.getEmitters().size(), 3u);
            EXPECT_FALSE(scene.getEmitters()[2].isFixed());
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
            const Index first = scene.addMesh(quads[0], {}, {}, Testing::sQuadIndices);
            const Index middle = scene.addMesh(triangleAt(5.0f), {}, {}, Testing::sTriangleIndices);
            const Index last = scene.addMesh(quads[1], {}, {}, Testing::sQuadIndices);

            ASSERT_EQ(scene.getPositions().size(), 11u);
            ASSERT_EQ(scene.getIndices().size(), 15u);
            ASSERT_EQ(scene.getMeshes()[last].mVertexOffset, 7u);

            const std::uint64_t was = scene.getStructureRevision();
            const std::array keep{ first, last };
            const std::array<Index, 0> noMaterials{};
            ASSERT_TRUE(scene.release(keep, noMaterials));

            // Nothing moved, nothing shrank, and every index still means what it meant.
            EXPECT_EQ(scene.getMeshes().size(), 3u);
            EXPECT_EQ(scene.getPositions().size(), 11u);
            EXPECT_EQ(scene.getIndices().size(), 15u);
            EXPECT_EQ(scene.getMeshes()[last].mVertexOffset, 7u);
            EXPECT_EQ(scene.getMeshPositions(first)[0].z(), 0.0f);
            EXPECT_EQ(scene.getMeshPositions(last)[0].z(), 2.0f);

            // The freed one describes nothing until something takes it, so a backend that walks the
            // table builds a structure over no triangles rather than over somebody else's.
            EXPECT_EQ(scene.getMeshes()[middle].mVertexCount, 0u);
            EXPECT_EQ(scene.getMeshes()[middle].mIndexCount, 0u);

            EXPECT_EQ(scene.getStructureRevision(), was)
                << "nothing arrived, so nothing built from these indices is out of date";

            // **The sweep names the slot it gave up, and it stops being an arrival by naming it.**
            // Nothing has been handed over, so all three are still spoken for — two as arrivals and
            // the third as a departure, never as both.
            EXPECT_EQ(sorted(scene.getFreedMeshes()), (std::vector<Index>{ middle }));
            EXPECT_EQ(sorted(scene.getArrivedMeshes()), (std::vector<Index>{ first, last }));

            // A triangle fits the hole exactly and takes it back, at the index and the offset the
            // old one had.
            const Index moved = scene.addMesh(triangleAt(5.0f), {}, {}, Testing::sTriangleIndices);
            EXPECT_EQ(moved, middle);
            EXPECT_EQ(scene.getMeshes()[moved].mVertexOffset, 4u);
            EXPECT_EQ(scene.getMeshes()[moved].mVertexCount, 3u);
            EXPECT_EQ(scene.getPositions().size(), 11u) << "a reused slot appended";
            EXPECT_GT(scene.getStructureRevision(), was) << "a slot taken over holds different geometry";

            // And the last mesh is still where it was, which a compaction is what would break.
            EXPECT_EQ(scene.getMeshPositions(last)[0].z(), 2.0f);

            // **Taking the slot back moves it the other way**, which is what lets a backend apply
            // the two lists in either order: this slot is built and not then destroyed, whichever
            // half it does first.
            EXPECT_EQ(sorted(scene.getArrivedMeshes()), (std::vector<Index>{ first, moved, last }));
            EXPECT_TRUE(scene.getFreedMeshes().empty()) << "a slot taken back was still reported as gone";

            scene.clearArrivals();
            EXPECT_TRUE(scene.getArrivedMeshes().empty());
            EXPECT_TRUE(scene.getFreedMeshes().empty());
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
            const Index slot = scene.addMesh(Testing::sUnitQuad, {}, {}, Testing::sQuadIndices);

            const std::uint64_t meshes = scene.getMeshRevision();
            const std::uint64_t structure = scene.getStructureRevision();

            // A texture is an upload, not a structure to build.
            scene.addTexture(VFS::Path::NormalizedView("textures/tx_stone.dds"));
            EXPECT_EQ(scene.getMeshRevision(), meshes) << "a texture asked for the structures to be built again";
            EXPECT_GT(scene.getStructureRevision(), structure);

            // The slot comes back and is taken over. The table is the same size it was, and what is
            // in it is not.
            ASSERT_TRUE(scene.release({}, {}));
            EXPECT_EQ(scene.getMeshRevision(), meshes) << "a cell leaving asked for the structures to be built again";

            EXPECT_EQ(scene.addMesh(triangleAt(5.0f), {}, {}, Testing::sTriangleIndices), slot);
            EXPECT_EQ(scene.getMeshes().size(), 1u) << "the table grew, so a size test would have caught this anyway";
            EXPECT_GT(scene.getMeshRevision(), meshes) << "a slot taken over went unnoticed";
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

            const Index roomy = scene.addMesh(big, {}, {}, bigIndices);
            const Index snug = scene.addMesh(Testing::sUnitQuad, {}, {}, Testing::sQuadIndices);
            const Index kept = scene.addMesh(Testing::sUnitQuad, {}, {}, Testing::sQuadIndices);

            ASSERT_EQ(scene.getMeshes()[roomy].mVertexOffset, 0u);
            ASSERT_EQ(scene.getMeshes()[snug].mVertexOffset, 8u);
            ASSERT_EQ(scene.getMeshes()[kept].mVertexOffset, 12u);

            const std::array keep{ kept };
            ASSERT_TRUE(scene.release(keep, {}));

            // Exactly the two that went, once each. Sorted, because which way a sweep walks its
            // table is not something a backend should have to know.
            EXPECT_EQ(sorted(scene.getFreedMeshes()), (std::vector<Index>{ roomy, snug }));

            const std::size_t vertices = scene.getPositions().size();
            ASSERT_EQ(vertices, 16u);

            // The quad takes the front of the merged hole and leaves eight vertices behind it.
            const Index quad = scene.addMesh(Testing::sUnitQuad, {}, {}, Testing::sQuadIndices);
            EXPECT_EQ(scene.getMeshes()[quad].mVertexOffset, 0u);

            // **Which is what the eight-vertex mesh then fits into.** Unmerged, the two holes were
            // eight and four and the four had just been spent, so this would have appended.
            const Index again = scene.addMesh(big, {}, {}, bigIndices);
            EXPECT_EQ(scene.getMeshes()[again].mVertexOffset, 4u);
            EXPECT_EQ(scene.getPositions().size(), vertices) << "a mesh that fitted a hole appended anyway";

            // Both freed slots have been taken, in the order they were given back.
            EXPECT_EQ(quad, snug);
            EXPECT_EQ(again, roomy);

            // Nothing fits now, so this one goes on the end.
            EXPECT_EQ(scene.addMesh(big, {}, {}, bigIndices), 3u);
            EXPECT_GT(scene.getPositions().size(), vertices);
        }

        /// A slot taken over holds its own attributes and none of its predecessor's.
        ///
        /// **The one way a reused slot can be quietly wrong.** A mesh that brings no normals is
        /// given zeroes on a fresh slot because the buffer was grown for it; on a reused one the
        /// room already holds whatever the last tenant put there, and a surface lit by somebody
        /// else's normals looks lit rather than looking broken.
        TEST(RtxSceneDescTest, aReusedSlotDoesNotInheritTheAttributesOfWhatStoodInIt)
        {
            SceneDesc scene;

            const std::array<osg::Vec3f, 4> normals{ osg::Vec3f(1.0f, 0.0f, 0.0f), osg::Vec3f(1.0f, 0.0f, 0.0f),
                osg::Vec3f(1.0f, 0.0f, 0.0f), osg::Vec3f(1.0f, 0.0f, 0.0f) };
            const std::array<osg::Vec2f, 4> uvs{ osg::Vec2f(0.5f, 0.5f), osg::Vec2f(0.5f, 0.5f), osg::Vec2f(0.5f, 0.5f),
                osg::Vec2f(0.5f, 0.5f) };

            const Index slot = scene.addMesh(Testing::sUnitQuad, normals, uvs, Testing::sQuadIndices);
            ASSERT_EQ(scene.getNormals()[scene.getMeshes()[slot].mVertexOffset], osg::Vec3f(1.0f, 0.0f, 0.0f));

            ASSERT_TRUE(scene.release({}, {}));
            EXPECT_EQ(scene.addMesh(Testing::sUnitQuad, {}, {}, Testing::sQuadIndices), slot);

            EXPECT_EQ(scene.getNormals()[scene.getMeshes()[slot].mVertexOffset], osg::Vec3f())
                << "the slot kept the last tenant's normals";
            EXPECT_EQ(scene.getTexCoords()[0], osg::Vec2f());
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

            Index mGround = mScene.addTexture(VFS::Path::NormalizedView("textures/tx_ground.dds"));
            Index mStone = mScene.addTexture(VFS::Path::NormalizedView("textures/tx_stone.dds"));
            Index mSand = mScene.addTexture(VFS::Path::NormalizedView("textures/tx_sand.dds"));
            Index mMoss = mScene.addTexture(VFS::Path::NormalizedView("textures/tx_moss.dds"));

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
                    .mMaskOffset = mScene.addMask(sGroundWeights),
                    .mMaskWidth = 2,
                    .mMaskHeight = 2 } };
                const Span droppedRun = mScene.addLayers(droppedLayers);
                mDropped = mScene.addMaterial(Material{ .mKind = MaterialKind::Terrain,
                    .mLayerOffset = droppedRun.mOffset,
                    .mLayerCount = droppedRun.mCount });

                mPlain = mScene.addMaterial(Material{ .mDiffuse = mStone });

                const std::array keptLayers{ MaterialLayer{ .mDiffuse = mSand,
                                                 .mMaskOffset = mScene.addMask(sSandWeights),
                                                 .mMaskWidth = 3,
                                                 .mMaskHeight = 3 },
                    MaterialLayer{ .mDiffuse = mMoss } };
                const Span keptRun = mScene.addLayers(keptLayers);
                mKept = mScene.addMaterial(Material{
                    .mKind = MaterialKind::Terrain, .mLayerOffset = keptRun.mOffset, .mLayerCount = keptRun.mCount });

                mLayersBefore = mScene.getLayers().size();
                mMasksBefore = mScene.getMasks().size();

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
            EXPECT_EQ(scene.getMaterials().size(), 3u);
            EXPECT_EQ(scene.getMaterials()[terrain.mPlain].mDiffuse, terrain.mStone);
            EXPECT_EQ(scene.getMaterials()[terrain.mKept].mLayerOffset, 1u);
            EXPECT_EQ(scene.getMaterials()[terrain.mKept].mLayerCount, 2u);

            EXPECT_EQ(scene.getLayers().size(), terrain.mLayersBefore)
                << "the tables never shrink, they are reused in place";
            EXPECT_EQ(scene.getMasks().size(), terrain.mMasksBefore);
            EXPECT_EQ(scene.getLayers()[1].mDiffuse, terrain.mSand);
            EXPECT_EQ(scene.getLayers()[2].mDiffuse, terrain.mMoss);

            // **The next chunk of the same shape lands in the hole the first one left.** One layer
            // and four weights, which is exactly what went: both come back at zero and neither table
            // is any longer than it was.
            const std::array arrivingLayers{ MaterialLayer{ .mDiffuse = terrain.mMoss,
                .mMaskOffset = scene.addMask(ReleasedTerrain::sGroundWeights),
                .mMaskWidth = 2,
                .mMaskHeight = 2 } };
            const Span arrivingRun = scene.addLayers(arrivingLayers);

            EXPECT_EQ(arrivingLayers[0].mMaskOffset, 0u) << "the freed mask run";
            EXPECT_EQ(arrivingRun, (Span{ .mOffset = 0, .mCount = 1 })) << "the freed layer run";
            EXPECT_EQ(scene.getLayers().size(), terrain.mLayersBefore)
                << "the layer table grew past a hole that fitted";
            EXPECT_EQ(scene.getMasks().size(), terrain.mMasksBefore) << "the mask table grew past a hole that fitted";

            // The freed slot goes to the next material asked for, whatever size it is: a material is
            // one size, so there is no fit to find.
            EXPECT_EQ(scene.addMaterial(Material{ .mDiffuse = terrain.mMoss }), terrain.mDropped);
            EXPECT_EQ(scene.getMaterials().size(), 3u);
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
            EXPECT_EQ(sorted(scene.getFreedTextures()), (std::vector<Index>{ terrain.mGround }));
            EXPECT_EQ(sorted(scene.getArrivedTextures()),
                (std::vector<Index>{ terrain.mStone, terrain.mSand, terrain.mMoss }));

            ASSERT_EQ(scene.getTextures().size(), 4u) << "the table shrank, so something was renumbered";
            EXPECT_TRUE(scene.getTextures()[terrain.mGround].value().empty()) << "a texture nothing wears was kept";

            // The three the survivors wear are untouched, at the indices they were given.
            EXPECT_EQ(scene.getTextures()[terrain.mStone], VFS::Path::NormalizedView("textures/tx_stone.dds"));
            EXPECT_EQ(scene.getTextures()[terrain.mSand], VFS::Path::NormalizedView("textures/tx_sand.dds"));
            EXPECT_EQ(scene.getTextures()[terrain.mMoss], VFS::Path::NormalizedView("textures/tx_moss.dds"));

            // The freed slot is what the next texture takes, and the path lookup went with it: asking
            // for `tx_ground` again is a new arrival rather than a hit on a slot nothing stands in.
            EXPECT_EQ(scene.addTexture(VFS::Path::NormalizedView("textures/tx_ground.dds")), terrain.mGround);
            EXPECT_EQ(scene.getTextures().size(), 4u) << "the table grew past a free slot";
            EXPECT_EQ(scene.getArrivedTextures().back(), terrain.mGround)
                << "a slot taken over was not reported as arriving";
            EXPECT_TRUE(scene.getFreedTextures().empty()) << "a slot taken back was still reported as gone";
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
            const Index mesh = scene.addMesh(Testing::sUnitQuad, {}, {}, Testing::sQuadIndices);
            const Index first = scene.addMaterial(Material{});

            const std::uint64_t structure = scene.getStructureRevision();
            scene.clearArrivals();

            // A second material, which is what a state set with a new address comes to.
            Material other;
            other.mTwoSided = true;
            const Index kept = scene.addMaterial(other);

            EXPECT_EQ(scene.getStructureRevision(), structure) << "a material asked for a rebuild";
            EXPECT_EQ(sorted(scene.getWrittenMaterials()), (std::vector<Index>{ kept }))
                << "the row that arrived, and only it";

            // And taking one away again is no shading change at all: nothing stands on the row, so
            // nothing reads it and nothing has to write it.
            scene.clearArrivals();
            const std::array meshes{ mesh };
            const std::array materials{ kept };

            ASSERT_TRUE(scene.release(meshes, materials));
            EXPECT_EQ(scene.getStructureRevision(), structure) << "a sweep of one material asked for a rebuild";
            EXPECT_TRUE(scene.getWrittenMaterials().empty()) << "a sweep reported a row to write";

            // **And a mesh going is no longer the other answer either.** It was, while a sweep
            // compacted: the table moved and everything built from it had to be built again. A slot
            // that is freed in place invalidates nothing, so the frame after a cell leaves costs the
            // top level and nothing else.
            const std::uint64_t before = scene.getStructureRevision();
            ASSERT_TRUE(scene.release({}, materials));
            EXPECT_EQ(scene.getStructureRevision(), before) << "a cell leaving asked for a rebuild";

            // **The slot the sweep freed is taken over, and that is a row again.** A flipbook added
            // and then rewritten on one frame is one row too: the list holds each slot once.
            EXPECT_EQ(scene.addMaterial(Material{}), first) << "a freed slot was not the one handed out";
            scene.setMaterial(first, other);
            EXPECT_EQ(sorted(scene.getWrittenMaterials()), (std::vector<Index>{ first }));

            // A rewrite that changes nothing is not a write, which is what a paused game is.
            scene.clearArrivals();
            scene.setMaterial(first, other);
            EXPECT_TRUE(scene.getWrittenMaterials().empty()) << "writing back what was there reported a row";
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
            const Index mask = scene.addMask(weights);
            EXPECT_EQ(mask, 0u);
            EXPECT_EQ(runs(scene.getArrivedMasks()), (std::vector<Span>{ Span{ .mOffset = 0, .mCount = 4 } }));

            const std::array layers{
                MaterialLayer{ .mMaskOffset = mask, .mMaskWidth = 2, .mMaskHeight = 2 },
                MaterialLayer{},
            };
            const Span run = scene.addLayers(layers);
            EXPECT_EQ(run, (Span{ .mOffset = 0, .mCount = 2 }));
            EXPECT_EQ(runs(scene.getArrivedLayers()), (std::vector<Span>{ run }));

            scene.addMaterial(
                Material{ .mKind = MaterialKind::Terrain, .mLayerOffset = run.mOffset, .mLayerCount = run.mCount });
            scene.clearArrivals();
            EXPECT_TRUE(scene.getArrivedMasks().empty());
            EXPECT_TRUE(scene.getArrivedLayers().empty());

            // A second chunk lands past the first: its runs are its own and say where they are.
            const std::array<float, 2> more{ 0.5f, 0.5f };
            EXPECT_EQ(scene.addMask(more), 4u);
            EXPECT_EQ(runs(scene.getArrivedMasks()), (std::vector<Span>{ Span{ .mOffset = 4, .mCount = 2 } }));

            const std::array one{ MaterialLayer{ .mMaskOffset = 4, .mMaskWidth = 2, .mMaskHeight = 1 } };
            EXPECT_EQ(scene.addLayers(one), (Span{ .mOffset = 2, .mCount = 1 }));
            EXPECT_EQ(runs(scene.getArrivedLayers()), (std::vector<Span>{ Span{ .mOffset = 2, .mCount = 1 } }));
            scene.clearArrivals();

            // The first chunk goes and its runs go with it — reported to nobody, because nothing
            // reads a run nothing names. The next chunk that fits lands in the hole, and that
            // arrival is what names the run again.
            const std::array<Index, 0> noMeshes{};
            const std::array keep{ scene.addMaterial(Material{}) };
            scene.clearArrivals();
            ASSERT_TRUE(scene.release(noMeshes, keep));
            EXPECT_TRUE(scene.getArrivedMasks().empty()) << "a sweep reported a run to write";
            EXPECT_TRUE(scene.getArrivedLayers().empty());
            EXPECT_TRUE(scene.getWrittenMaterials().empty());

            EXPECT_EQ(scene.addMask(weights), 0u) << "the freed run was not the one handed out";
            EXPECT_EQ(runs(scene.getArrivedMasks()), (std::vector<Span>{ Span{ .mOffset = 0, .mCount = 4 } }));
        }

        /// A scene that lost nothing is left entirely alone, and a sprite's texture is the caller's
        /// to speak for.
        TEST(RtxSceneDescTest, releasingDoesNothingWhenNothingWent)
        {
            SceneDesc scene;
            const Index mesh = scene.addMesh(Testing::sUnitQuad, {}, {}, Testing::sQuadIndices);
            const Index material = scene.addMaterial(Material{});
            scene.addTexture(VFS::Path::NormalizedView("textures/tx_fire_00.dds"));

            const std::array meshes{ mesh };
            const std::array materials{ material };
            const std::uint64_t was = scene.getStructureRevision();
            scene.clearArrivals();

            EXPECT_FALSE(scene.release(meshes, materials));
            EXPECT_EQ(scene.getStructureRevision(), was);
            EXPECT_TRUE(scene.getWrittenMaterials().empty());
            EXPECT_TRUE(scene.getFreedMeshes().empty()) << "a sweep that freed nothing named something";
            EXPECT_TRUE(scene.getFreedTextures().empty());

            // Asked again with everything already free, which is the frame after a cell left: the
            // live count is what the keep set is compared against, not the table's size.
            const std::array<Index, 0> none{};
            ASSERT_TRUE(scene.release(none, none));
            EXPECT_EQ(sorted(scene.getFreedMeshes()), (std::vector<Index>{ mesh }));

            EXPECT_FALSE(scene.release(none, none)) << "a table with nothing left in it went again";
            EXPECT_EQ(sorted(scene.getFreedMeshes()), (std::vector<Index>{ mesh })) << "a slot went twice";

            // A texture nothing has been told to name is nobody's to give back, so it stays — which
            // is what `addTexture` says of a caller that asks for one and then puts it nowhere.
            EXPECT_EQ(scene.getTextures().size(), 1u);
            EXPECT_TRUE(scene.getFreedTextures().empty());
        }

        /// A sweep leaves the per-frame lists as the walk left them, because the frame it happens on
        /// is about to be drawn from them. Emptying them here left every lamp in the world dark for
        /// exactly one frame, on the frames a sweep freed something.
        TEST(RtxSceneDescTest, aSweepLeavesTheListsTheWalkFilled)
        {
            SceneDesc scene;
            const Index kept = scene.addMesh(Testing::sUnitQuad, {}, {}, Testing::sQuadIndices);
            scene.addMesh(Testing::sUnitQuad, {}, {}, Testing::sQuadIndices);

            scene.addLight(Light{ .mPosition = osg::Vec3f(1.0f, 2.0f, 3.0f),
                .mIntensity = osg::Vec3f(4.0f, 5.0f, 6.0f),
                .mReach = 256.0f });
            scene.addLight(Light{ .mPosition = osg::Vec3f(-7.0f, 8.0f, 9.0f),
                .mIntensity = osg::Vec3f(1.0f, 1.0f, 1.0f),
                .mReach = 512.0f });

            const std::array meshes{ kept };
            const std::array<Index, 0> noMaterials{};
            ASSERT_TRUE(scene.release(meshes, noMaterials)) << "the second mesh should have gone";

            ASSERT_EQ(scene.getLights().size(), 2u) << "the sweep emptied the light table the walk had just filled";
            EXPECT_EQ(scene.getLights()[0].mPosition, osg::Vec3f(1.0f, 2.0f, 3.0f));
            EXPECT_EQ(scene.getLights()[0].mReach, 256.0f);
            EXPECT_EQ(scene.getLights()[1].mPosition, osg::Vec3f(-7.0f, 8.0f, 9.0f));
            EXPECT_EQ(scene.getLights()[1].mReach, 512.0f);

            // Emptying them is still `clearPlacement`'s, which is what the next walk begins with.
            scene.clearPlacement();
            EXPECT_TRUE(scene.getLights().empty());
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
            const Index mesh = scene.addMesh(Testing::sUnitQuad, {}, {}, Testing::sQuadIndices);
            const Index shared = scene.addTexture(VFS::Path::NormalizedView("textures/tx_stone.dds"));
            const Index lone = scene.addTexture(VFS::Path::NormalizedView("textures/tx_sand.dds"));

            scene.addMaterial(Material{ .mDiffuse = shared });
            const Index second = scene.addMaterial(Material{ .mDiffuse = shared, .mNormal = lone });

            const std::array meshes{ mesh };
            const std::array keepSecond{ second };
            ASSERT_TRUE(scene.release(meshes, keepSecond));

            EXPECT_TRUE(scene.getFreedTextures().empty()) << "a texture another material still names";
            EXPECT_EQ(scene.getTextures()[shared], VFS::Path::NormalizedView("textures/tx_stone.dds"));

            const std::array<Index, 0> none{};
            ASSERT_TRUE(scene.release(meshes, none));

            EXPECT_EQ(sorted(scene.getFreedTextures()), (std::vector<Index>{ shared, lone }));
            EXPECT_TRUE(scene.getTextures()[shared].value().empty());
            EXPECT_TRUE(scene.getTextures()[lone].value().empty());
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

            const Index baked = scene.addBakedTexture("composite/-3,-2/2");
            ASSERT_EQ(baked, 0u);

            // Standing, and standing is not free — the path is empty because it has none, which is
            // the same thing a free slot's path says and not the same fact.
            EXPECT_FALSE(scene.isTextureFree(baked));
            EXPECT_TRUE(scene.getTextures()[baked].value().empty()) << "it came from no file";
            EXPECT_EQ(scene.getBakedTextures()[baked], "composite/-3,-2/2");

            // The key is what makes two chunks that would bake the same image share one slot.
            EXPECT_EQ(scene.addBakedTexture("composite/-3,-2/2"), baked) << "the same bake took a second slot";
            EXPECT_EQ(scene.getTextures().size(), 1u);

            // A file beside it, so the free list has to hand back the right one.
            const Index file = scene.addTexture(VFS::Path::NormalizedView("textures/tx_stone.dds"));
            ASSERT_EQ(file, 1u);

            scene.holdTexture(baked);
            scene.dropTexture(baked);

            EXPECT_TRUE(scene.isTextureFree(baked)) << "nothing names it and it is still standing";
            EXPECT_TRUE(scene.getBakedTextures()[baked].empty());
            EXPECT_EQ(sorted(scene.getFreedTextures()), (std::vector<Index>{ baked }));

            // And the slot comes back, to a file this time — a freed slot is a row and not a kind.
            const Index next = scene.addTexture(VFS::Path::NormalizedView("textures/tx_sand.dds"));
            EXPECT_EQ(next, baked) << "the table grew past a free slot";
            EXPECT_EQ(scene.getTextures().size(), 2u);
            EXPECT_EQ(scene.getTextures()[next], VFS::Path::NormalizedView("textures/tx_sand.dds"));
            EXPECT_TRUE(scene.getBakedTextures()[next].empty()) << "the slot kept what the last tenant was";

            // The key is free again too, or a bake that came back would find a slot somebody else has.
            const Index again = scene.addBakedTexture("composite/-3,-2/2");
            EXPECT_EQ(again, 2u) << "a key the table gave back found a slot somebody else has";
            EXPECT_FALSE(scene.isTextureFree(file)) << "the file beside it was never touched";
        }

        /// A material rewritten gives back what it stopped naming and keeps what it still names.
        ///
        /// **What a flipbook is**: `NifOsg` turns a fire over thirty-two times a second by rewriting
        /// one state set, and the surface wearing it never moves. The material keeps its slot; the
        /// image it walked away from does not.
        TEST(RtxSceneDescTest, aMaterialRewrittenGivesBackOnlyWhatItStoppedNaming)
        {
            SceneDesc scene;
            const Index first = scene.addTexture(VFS::Path::NormalizedView("textures/tx_fire_00.dds"));
            const Index second = scene.addTexture(VFS::Path::NormalizedView("textures/tx_fire_01.dds"));
            const Index material = scene.addMaterial(Material{ .mDiffuse = first });

            scene.setMaterial(material, Material{ .mDiffuse = second });

            EXPECT_EQ(sorted(scene.getFreedTextures()), (std::vector<Index>{ first }));
            EXPECT_TRUE(scene.getTextures()[first].value().empty()) << "the frame it left is still named";
            EXPECT_EQ(scene.getTextures()[second], VFS::Path::NormalizedView("textures/tx_fire_01.dds"));

            // **And round again onto a frame it already had.** Taking the new set before giving the
            // old one back is the whole of what stops this: the other order takes the slot to zero,
            // empties its path and hands it to the next thing that asks for one — a texture changing
            // identity under a material that never stopped naming it.
            scene.setMaterial(material, Material{ .mDiffuse = second, .mTwoSided = true });

            EXPECT_EQ(scene.getTextures()[second], VFS::Path::NormalizedView("textures/tx_fire_01.dds"))
                << "a texture the material still names was let go and taken again";
            EXPECT_EQ(sorted(scene.getFreedTextures()), (std::vector<Index>{ first })) << "and reported as going";
        }

        /// A hold speaks for a texture no material can, and the slot goes when the hold does.
        TEST(RtxSceneDescTest, aHeldTextureGoesWhenTheHoldDoesAndNotBefore)
        {
            SceneDesc scene;
            const Index mesh = scene.addMesh(Testing::sUnitQuad, {}, {}, Testing::sQuadIndices);
            const Index material = scene.addMaterial(Material{});
            const Index sprite = scene.addTexture(VFS::Path::NormalizedView("textures/tx_fire_00.dds"));
            scene.holdTexture(sprite);

            // The ordinary frame, where the sweep answers with two comparisons and returns.
            const std::array meshes{ mesh };
            const std::array materials{ material };
            EXPECT_FALSE(scene.release(meshes, materials));
            EXPECT_EQ(scene.getTextures()[sprite], VFS::Path::NormalizedView("textures/tx_fire_00.dds"));

            scene.dropTexture(sprite);

            EXPECT_EQ(sorted(scene.getFreedTextures()), (std::vector<Index>{ sprite }));
            EXPECT_TRUE(scene.getTextures()[sprite].value().empty());

            // And the slot is handed out again rather than the table growing.
            EXPECT_EQ(scene.addTexture(VFS::Path::NormalizedView("textures/tx_smoke.dds")), sprite);
            EXPECT_EQ(scene.getTextures().size(), 1u);
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
            const Index first = scene.addMesh(vertices(200000), {}, {}, triangle);
            EXPECT_EQ(scene.getMeshes()[first].mVertexOffset, 0u);

            const Index second = scene.addMesh(vertices(100000), {}, {}, triangle);
            EXPECT_EQ(scene.getMeshes()[second].mVertexOffset, SceneDesc::sVertexBlock)
                << "a run was laid across a block boundary";
            EXPECT_EQ(scene.getPositions().size(), std::size_t{ 362144 });

            // And the 62,144 the second one stepped over is a hole like any other.
            const Index third = scene.addMesh(vertices(60000), {}, {}, triangle);
            EXPECT_EQ(scene.getMeshes()[third].mVertexOffset, 200000u) << "the tail of a block was not reused";
            EXPECT_EQ(scene.getPositions().size(), std::size_t{ 362144 }) << "a mesh that fitted the tail appended";

            // None of the three crosses a boundary, which is the property rather than the three
            // offsets that happen to demonstrate it.
            for (const Index mesh : { first, second, third })
            {
                const MeshRange& range = scene.getMeshes()[mesh];
                EXPECT_EQ(range.mVertexOffset / SceneDesc::sVertexBlock,
                    (range.mVertexOffset + range.mVertexCount - 1) / SceneDesc::sVertexBlock)
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
            EXPECT_THROW(scene.addMesh(tooMany, {}, {}, triangle), Error);

            // And exactly a block is not too many, so the refusal is a boundary and not a ban.
            EXPECT_NO_THROW(scene.addMesh(std::span(tooMany).first(SceneDesc::sVertexBlock), {}, {}, triangle));
        }

        TEST(RtxSceneDescTest, clearingEmptiesEveryTable)
        {
            SceneDesc scene;
            const Index mesh = scene.addMesh(Testing::sUnitQuad, {}, {}, Testing::sQuadIndices);
            const Index material = scene.addMaterial(Material{});
            scene.addTexture(VFS::Path::NormalizedView("textures/tx_stone_01.dds"));
            scene.addBakedTexture("composite/0,0/1");
            scene.addInstance(
                MeshInstance{ .mTransform = osg::Matrixf::identity(), .mMesh = mesh, .mMaterial = material });
            const Index body = scene.addMesh(
                Testing::sUnitQuad, {}, {}, Testing::sQuadIndices, {}, Deform::Rig, Testing::addOneBoneRig(scene, 4));
            scene.poseRig(body, std::array{ boneUp(1.0f) }, osg::BoundingBoxf());
            scene.addEmitter(std::array{ Sprite{ .mRadius = 1.0f } }, 0, true);

            scene.clear();

            EXPECT_TRUE(scene.getMeshes().empty());
            EXPECT_TRUE(scene.getInstances().empty());
            EXPECT_TRUE(scene.getMaterials().empty());
            EXPECT_TRUE(scene.getTextures().empty());
            EXPECT_TRUE(scene.getBakedTextures().empty());
            EXPECT_TRUE(scene.getPositions().empty());
            EXPECT_TRUE(scene.getDeformed().empty());
            EXPECT_TRUE(scene.getRigs().empty());
            EXPECT_TRUE(scene.getRuns().empty());
            EXPECT_TRUE(scene.getInfluences().empty());
            EXPECT_TRUE(scene.getBones().empty());
            EXPECT_TRUE(scene.getArrivedRigs().empty());
            EXPECT_EQ(scene.getBindVertexCount(), 0u);
            EXPECT_TRUE(scene.getSprites().empty());
            EXPECT_TRUE(scene.getEmitters().empty());
            EXPECT_EQ(scene.getTriangleCount(), 0u);

            // A reset renumbers, so nothing that arrived or went under the old numbering means
            // anything: a backend hearing this rebuilds rather than applying either list.
            EXPECT_TRUE(scene.getArrivedMeshes().empty());
            EXPECT_TRUE(scene.getFreedMeshes().empty());
            EXPECT_TRUE(scene.getArrivedTextures().empty());
            EXPECT_TRUE(scene.getFreedTextures().empty());

            // And the lookups with them, or a key from the world before this one finds a slot in the
            // world after it.
            EXPECT_EQ(scene.addBakedTexture("composite/0,0/1"), 0u);
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

            const Index quad = scene.addMesh(Testing::sUnitQuad, {}, {}, Testing::sQuadIndices);
            const Index ground = scene.addMaterial(Material{ .mKind = MaterialKind::Terrain });
            const Index sea = scene.addMaterial(Material{ .mKind = MaterialKind::Water });

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
            const Index wide = scene.addMesh(Testing::sUnitQuad, {}, {}, Testing::sQuadIndices);
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

            const Index quad = scene.addMesh(
                Testing::sUnitQuad, {}, {}, Testing::sQuadIndices, {}, Deform::Rig, Testing::addOneBoneRig(scene, 4));
            const Index material = scene.addMaterial(Material{});
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