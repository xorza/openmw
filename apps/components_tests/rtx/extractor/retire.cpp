#include "fixture.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <osg/Array>
#include <osg/Matrix>
#include <osg/MatrixTransform>
#include <osg/Vec3f>
#include <osg/Vec4f>
#include <osg/observer_ptr>
#include <osg/ref_ptr>

#include <components/rtx/deformertable.hpp>
#include <components/rtx/instancerecord.hpp>
#include <components/rtx/material.hpp>
#include <components/rtx/mesh.hpp>
#include <components/rtx/runs.hpp>
#include <components/rtx/shaders/scene.h>
#include <components/rtx/shapefold.hpp>
#include <components/sceneutil/stableidentity.hpp>
#include <components/vfs/pathutil.hpp>

namespace Rtx::Testing
{
    namespace
    {
        /// What the walk stopped finding leaves the scene, and what stayed keeps working.
        ///
        /// **The whole reason this exists is not memory but identity.** The mesh cache is keyed on
        /// the `osg::Drawable*`, which is what makes a crate met in a second cell resolve to the
        /// crate already uploaded — and an address the engine freed when a cell unloaded can be
        /// handed straight back for something else. Sweeping is what stops the next thing allocated
        /// there inheriting a mesh it has nothing to do with.
        ///
        /// **The torn figure a change of clothes produced.** `NpcAnimation::updateParts` frees the
        /// body parts that changed and builds their replacements, and the allocator is free to put a
        /// new part exactly where a retired one was; a map keyed on the bare address then finds the
        /// retired part's entry under the new part's and mirrors geometry that has nothing to do with
        /// it. The entry owns its subject, so that address is not available to hand out again until
        /// the sweep lets go — which is what makes the identity true rather than likely.
        TEST_F(RtxSceneExtractorTest, aDrawableTheGraphLetGoKeepsItsAddressUntilTheSweepReleasesIt)
        {

            osg::ref_ptr<osg::Group> root = new osg::Group;
            osg::ref_ptr<osg::Geometry> part = makeQuad();
            root->addChild(part);

            walk(*root);
            ASSERT_EQ(mScene.meshes().getRows().size(), 1u);

            // The epoch this opens is what the walk below is measured against, so the sweep at the
            // end has something to find stale.
            ASSERT_TRUE(mExtractor.retire().empty());

            const osg::Geometry* was = part.get();
            osg::observer_ptr<osg::Geometry> watch = part;

            // The graph lets go, and so does the test. Nothing outside the extractor holds it now.
            root->removeChild(part);
            part = nullptr;
            ASSERT_EQ(was->referenceCount(), 1) << "something other than the identity map is holding it";
            ASSERT_TRUE(watch.valid()) << "the map let it go while its entry still stood";

            // So the replacement cannot land where it was, which is the whole of the fix: the
            // address is spoken for.
            osg::ref_ptr<osg::Geometry> replacement = makeQuad();
            static_cast<osg::Vec3Array*>(replacement->getVertexArray())->at(0).z() = 5.0f;
            ASSERT_NE(replacement.get(), was) << "the replacement landed on the retired part's address";

            root->addChild(replacement);
            mScene.clearPlacement();

            const ExtractionStats again = walk(*root, 0, 1);
            EXPECT_EQ(again.mMeshesAdded, 1u) << "the replacement resolved to the retired part's mesh";
            EXPECT_EQ(again.mMeshesReused, 0u);

            // Two slots, and the new one carries its own vertices rather than the retired one's.
            ASSERT_EQ(mScene.meshes().getRows().size(), 2u);
            EXPECT_EQ(mScene.meshes().getMeshPositions(1)[0].z(), 5.0f);

            // **And the sweep is what lets go.** Holding the key is what costs: geometry the graph
            // dropped outlives its owner until here, and a caller that never sweeps holds every
            // drawable it has ever walked.
            const Retirement went = mExtractor.retire();
            EXPECT_EQ(went.mMeshes, 1u);
            EXPECT_FALSE(watch.valid()) << "the sweep dropped the entry and kept the drawable alive";
        }

        /// **The same figure, torn one level up, and the walk holds nothing for it.** A placement is
        /// known by where its drawable stands in the structure under the nearest stamped node —
        /// which child of which child — and never by a node's address. So a part the engine frees
        /// and builds again in the same place, as `NpcAnimation::updateParts` does for a body part
        /// whose mesh the cache shares, is the placement it replaces, moved: it keeps its slot and
        /// the history a reprojection reads. Nothing is held to make that true, so the freed node
        /// goes the moment the graph lets it go.
        TEST_F(RtxSceneExtractorTest, aNodeRebuiltInItsPlaceIsThePlacementItReplaces)
        {
            osg::ref_ptr<osg::Group> root = new osg::Group;
            osg::ref_ptr<osg::Geometry> shared = makeQuad();
            osg::ref_ptr<osg::MatrixTransform> part = new osg::MatrixTransform(osg::Matrix::translate(1.0, 0.0, 0.0));
            part->addChild(shared);
            root->addChild(part);

            walk(*root);
            ASSERT_EQ(mScene.placements().getRows().size(), 1u);
            ASSERT_TRUE(mExtractor.retire().empty());
            mScene.placements().advance();

            osg::observer_ptr<osg::MatrixTransform> watch = part;

            // The graph lets go of the node and keeps the drawable. Nothing holds the node now.
            root->removeChild(part);
            part = nullptr;
            EXPECT_FALSE(watch.valid()) << "the walk held a node the graph let go of";

            osg::ref_ptr<osg::MatrixTransform> replacement
                = new osg::MatrixTransform(osg::Matrix::translate(2.0, 0.0, 0.0));
            replacement->addChild(shared);
            root->addChild(replacement);

            mScene.clearPlacement();
            const ExtractionStats again = walk(*root, 0, 1);

            // The one placement, moved from where the part stood to where its replacement does.
            EXPECT_EQ(again.mMeshesReused, 1u);
            EXPECT_EQ(again.mInstances, 1u);
            EXPECT_EQ(again.mRestood, 0u);
            ASSERT_EQ(mScene.placements().getRows().size(), 1u);
            ASSERT_EQ(mScene.placements().getMoved().size(), 1u);
            EXPECT_EQ(placedAt(mScene, 0), osg::Vec3f(2.0f, 0.0f, 0.0f));
            EXPECT_EQ(mScene.placements().getRows()[0].mPrevious.getTrans(), osg::Vec3f(1.0f, 0.0f, 0.0f));

            EXPECT_TRUE(mExtractor.retire().empty()) << "nothing went: the placement was carried";
            EXPECT_EQ(mScene.placements().getCounts().mPlaced, 1u);
        }

        /// **A stamped node is known by its stamp, wherever it sits among its siblings.** The engine
        /// stamps every reference root, so a crate is the same placement after the crate before it
        /// in the cell has gone — which under a structural identity alone it would not be.
        TEST_F(RtxSceneExtractorTest, aStampedNodeKeepsItsPlacementWhenTheSiblingBeforeItGoes)
        {
            osg::ref_ptr<osg::Group> root = new osg::Group;
            osg::ref_ptr<osg::Geometry> shared = makeQuad();
            osg::ref_ptr<osg::MatrixTransform> first = new osg::MatrixTransform(osg::Matrix::translate(1.0, 0.0, 0.0));
            osg::ref_ptr<osg::MatrixTransform> second = new osg::MatrixTransform(osg::Matrix::translate(5.0, 0.0, 0.0));
            first->addChild(shared);
            second->addChild(shared);
            SceneUtil::StableIdentity::stamp(*first, 7);
            SceneUtil::StableIdentity::stamp(*second, 8);
            root->addChild(first);
            root->addChild(second);
            mExtractor.setStampDepth(1);

            walk(*root);
            ASSERT_EQ(mScene.placements().getRows().size(), 2u);
            ASSERT_EQ(placedAt(mScene, 1), osg::Vec3f(5.0f, 0.0f, 0.0f));
            ASSERT_TRUE(mExtractor.retire().empty());
            mScene.placements().advance();

            root->removeChild(first);
            mScene.clearPlacement();
            const ExtractionStats again = walk(*root, 0, 1);

            // The second stands where it stood, in its own slot, and did not move: it is not the
            // first's placement carried to a new place.
            EXPECT_EQ(again.mInstances, 1u);
            EXPECT_EQ(again.mRestood, 0u);
            EXPECT_TRUE(mScene.placements().getMoved().empty()) << "a still crate reported a move";
            EXPECT_TRUE(mScene.placements().getRows()[1].mInstance.isPlaced());

            mExtractor.retire();
            EXPECT_FALSE(mScene.placements().getRows()[0].mInstance.isPlaced()) << "the first's slot was kept";
            EXPECT_EQ(mScene.placements().getCounts().mPlaced, 1u);
        }

        /// **A stamp deeper than the walk was told to look is not read**, so the same two crates
        /// under a walk told nought are known by their places alone: the second is walked as the
        /// first, moved. The depth is what keeps the look off the fifty thousand nodes under the
        /// stamps, and this is what says the look actually stops there.
        TEST_F(RtxSceneExtractorTest, aStampBelowTheStatedDepthIsNotRead)
        {
            osg::ref_ptr<osg::Group> root = new osg::Group;
            osg::ref_ptr<osg::Geometry> shared = makeQuad();
            osg::ref_ptr<osg::MatrixTransform> first = new osg::MatrixTransform(osg::Matrix::translate(1.0, 0.0, 0.0));
            osg::ref_ptr<osg::MatrixTransform> second = new osg::MatrixTransform(osg::Matrix::translate(5.0, 0.0, 0.0));
            first->addChild(shared);
            second->addChild(shared);
            SceneUtil::StableIdentity::stamp(*first, 7);
            SceneUtil::StableIdentity::stamp(*second, 8);
            root->addChild(first);
            root->addChild(second);
            ASSERT_EQ(mExtractor.getStampDepth(), 0u) << "a fresh extractor reads stamps on the root alone";

            walk(*root);
            ASSERT_TRUE(mExtractor.retire().empty());
            mScene.placements().advance();

            root->removeChild(first);
            mScene.clearPlacement();
            walk(*root, 0, 1);

            ASSERT_EQ(mScene.placements().getMoved().size(), 1u);
            EXPECT_EQ(mScene.placements().getMoved()[0], 0u);
            EXPECT_EQ(placedAt(mScene, 0), osg::Vec3f(5.0f, 0.0f, 0.0f));
        }

        /// **And an unstamped node is known by its place, so a sibling that shifts into another's
        /// place takes over that placement.** The trade the structural identity makes, said out
        /// loud: two like parts under one unstamped node and the first goes, the second is walked as
        /// the first, moved. One frame of history read from where the first stood, for the siblings
        /// of one node — and no address, no hold and no allocator anywhere in the answer.
        TEST_F(RtxSceneExtractorTest, anUnstampedNodeIsKnownByItsPlaceAmongItsSiblings)
        {
            osg::ref_ptr<osg::Group> root = new osg::Group;
            osg::ref_ptr<osg::Geometry> shared = makeQuad();
            osg::ref_ptr<osg::MatrixTransform> first = new osg::MatrixTransform(osg::Matrix::translate(1.0, 0.0, 0.0));
            osg::ref_ptr<osg::MatrixTransform> second = new osg::MatrixTransform(osg::Matrix::translate(5.0, 0.0, 0.0));
            first->addChild(shared);
            second->addChild(shared);
            root->addChild(first);
            root->addChild(second);

            walk(*root);
            ASSERT_EQ(mScene.placements().getRows().size(), 2u);
            ASSERT_TRUE(mExtractor.retire().empty());
            mScene.placements().advance();

            root->removeChild(first);
            mScene.clearPlacement();
            const ExtractionStats again = walk(*root, 0, 1);

            EXPECT_EQ(again.mInstances, 1u);
            EXPECT_EQ(again.mRestood, 0u);
            ASSERT_EQ(mScene.placements().getMoved().size(), 1u);
            EXPECT_EQ(mScene.placements().getMoved()[0], 0u);
            EXPECT_EQ(placedAt(mScene, 0), osg::Vec3f(5.0f, 0.0f, 0.0f));
            EXPECT_EQ(mScene.placements().getRows()[0].mPrevious.getTrans(), osg::Vec3f(1.0f, 0.0f, 0.0f));

            mExtractor.retire();
            EXPECT_FALSE(mScene.placements().getRows()[1].mInstance.isPlaced()) << "the second's old slot was kept";
        }

        /// A mesh and a material of the scene's own, which no drawable names.
        ///
        /// What the cell ring does for a cell's ground: the rows are added straight to the scene,
        /// held on it, and let go of by giving the holds back — outside any walk, which is when a
        /// detached world lets go of everything.
        class OwnedRows
        {
        public:
            explicit OwnedRows(SceneDesc& scene)
                : mScene(scene)
            {
            }

            void stand()
            {
                const std::array<osg::Vec3f, 3> corners{ osg::Vec3f(0.0f, 0.0f, 0.0f), osg::Vec3f(1.0f, 0.0f, 0.0f),
                    osg::Vec3f(0.0f, 1.0f, 0.0f) };
                const std::array<std::uint32_t, 3> triangle{ 0, 1, 2 };

                mMaterial = mScene.addMaterial(Material{ .mKind = MaterialKind::Terrain });
                mMesh = mScene.addMesh(MeshArrays{ .mPositions = corners, .mIndices = triangle });
                mSlot = mScene.addInstance(MeshInstance{ .mMesh = mMesh, .mMaterial = mMaterial });
                mScene.meshes().hold(mMesh);
                mScene.materials().hold(mMaterial);
            }

            void letGo()
            {
                mScene.placements().drop(mSlot, Stander::Walk);
                mSlot = sNoIndex;
                mScene.meshes().drop(mMesh);
                mScene.materials().drop(mMaterial);
            }

            Index getMesh() const { return mMesh; }

        private:
            SceneDesc& mScene;
            Index mMesh = sNoIndex;
            Index mMaterial = sNoIndex;
            Index mSlot = sNoIndex;
        };

        /// **A row something holds survives every sweep, and goes on the first after the hold is
        /// given back.** The identity maps hold nothing for it, so without the hold the sweep after
        /// the first walk would release the ground under the player's feet — and without the scene
        /// saying a hold went, a sweep on a frame where every map stood whole would never run at
        /// all.
        TEST_F(RtxSceneExtractorTest, aRowAHoldKeepsIsKeptWhileHeldAndReleasedWhenLetGo)
        {
            OwnedRows rows(mScene);
            rows.stand();
            ASSERT_EQ(mScene.meshes().getLiveCount(), 1u);

            osg::ref_ptr<osg::Group> nothing = new osg::Group;
            mExtractor.extract(*nothing, osg::Matrixf::identity(), 0, 1);

            EXPECT_TRUE(mExtractor.retire().empty()) << "a held row is a survivor";
            EXPECT_EQ(mScene.meshes().getLiveCount(), 1u);

            // A second walk with nothing else in it: every map stands whole, and the row still
            // stands.
            mExtractor.extract(*nothing, osg::Matrixf::identity(), 0, 2);
            EXPECT_TRUE(mExtractor.retire().empty());
            EXPECT_EQ(mScene.meshes().getLiveCount(), 1u);

            // Let go of between walks, as a detached world does, and gone on the sweep after the
            // next — whose maps stand whole, so it is the dropped hold alone that runs it.
            rows.letGo();
            EXPECT_TRUE(mScene.hasDroppedHolds());
            mExtractor.extract(*nothing, osg::Matrixf::identity(), 0, 3);

            const Retirement went = mExtractor.retire();
            EXPECT_EQ(went.mMeshes, 1u);
            EXPECT_EQ(went.mMaterials, 1u);
            EXPECT_EQ(mScene.meshes().getLiveCount(), 0u) << "the row nothing holds was released";
            EXPECT_EQ(mScene.materials().getLiveCount(), 0u);
            EXPECT_FALSE(mScene.hasDroppedHolds());
            EXPECT_TRUE(mScene.isEmpty()) << "a scene whose last rows were released still stands something";

            // **A live row nothing holds is what no sweep can reach**, and the question the retire
            // asks of the scene at its end. The texture table has no sweep at all: a slot taken and
            // not held on the next line would carry its image for the life of the scene, and only
            // this would say so.
            EXPECT_TRUE(mScene.isConsistent());
            const Index orphan = mScene.textures().add(VFS::Path::NormalizedView("textures/forgotten.dds"));
            EXPECT_FALSE(mScene.isConsistent()) << "a live texture with no hold was not reported";
            mScene.textures().hold(orphan);
            EXPECT_TRUE(mScene.isConsistent());
            mScene.textures().drop(orphan);
            EXPECT_TRUE(mScene.isEmpty());
        }

        TEST_F(RtxSceneExtractorTest, aSweepDropsWhatTheWalkNoLongerFindsAndCarriesTheRest)
        {
            osg::ref_ptr<osg::Geometry> stays = makeQuad();
            osg::ref_ptr<osg::Geometry> goes = makeQuad();
            osg::ref_ptr<osg::Geometry> alsoStays = makeQuad();

            // Told apart by their vertices, so the survivors can be checked by what came out of them
            // rather than only by how many there are.
            static_cast<osg::Vec3Array*>(alsoStays->getVertexArray())->at(0).z() = 7.0f;

            osg::ref_ptr<osg::Group> whole = new osg::Group;
            whole->addChild(stays);
            whole->addChild(goes);
            whole->addChild(alsoStays);

            walk(*whole);
            ASSERT_EQ(mScene.meshes().getRows().size(), 3u);

            // Nothing has gone yet, so the sweep is a no-op — and the epoch it opens is what the
            // next walk is measured against.
            EXPECT_TRUE(mExtractor.retire().empty());
            EXPECT_EQ(mScene.meshes().getRows().size(), 3u);

            osg::ref_ptr<osg::Group> less = new osg::Group;
            less->addChild(stays);
            less->addChild(alsoStays);

            mScene.clearPlacement();
            walk(*less);

            const Retirement went = mExtractor.retire();
            EXPECT_EQ(went.mMeshes, 1u);
            EXPECT_EQ(went.mMaterials, 0u) << "an untextured quad has no state set and so no material";

            // **The table is the same size and the survivors are where they were.** Freeing a slot
            // in place is what lets a cell leave without renumbering every mesh in the world, and
            // renumbering is what made a boundary cost a full rebuild.
            ASSERT_EQ(mScene.meshes().getRows().size(), 3u);
            EXPECT_EQ(mScene.meshes().getRows()[1].mVertices.mCount, 0u) << "the middle slot should be free";
            EXPECT_EQ(mScene.meshes().getMeshPositions(2)[0].z(), 7.0f) << "a survivor moved";

            mScene.clearPlacement();
            const ExtractionStats after = walk(*less);

            EXPECT_EQ(after.mMeshesAdded, 0u) << "a survivor was re-added rather than recognised";
            EXPECT_EQ(after.mMeshesReused, 2u);

            // **Three slots and two standing in them.** A placement is its place in the structure:
            // the walk under `less` finds the first quad where it was, and in the second's place a
            // different mesh, so that placement is stood again in the slot it freed. The third
            // quad's old place is what nothing walks any more, and the sweep took it. A dropped
            // placement leaves its slot behind rather than closing the gap.
            ASSERT_EQ(mScene.placements().getCounts().mPlaced, 2u);
            ASSERT_EQ(mScene.placements().getRows().size(), 3u);
            EXPECT_FALSE(mScene.placements().getRows()[2].mInstance.isPlaced()) << "slot 2 should be a gap";

            // And what those placements name is what the walk resolved: the third quad is still
            // mesh two, where it was put.
            ASSERT_TRUE(mScene.placements().getRows()[0].mInstance.isPlaced());
            ASSERT_TRUE(mScene.placements().getRows()[1].mInstance.isPlaced());
            EXPECT_EQ(mScene.placements().getRows()[0].mInstance.mMesh, 0u);
            EXPECT_EQ(mScene.placements().getRows()[1].mInstance.mMesh, 2u);

            // The freed slot goes to the next quad that turns up, which is the same size as the one
            // that left it.
            osg::ref_ptr<osg::Geometry> arrives = makeQuad();
            osg::ref_ptr<osg::Group> more = new osg::Group;
            more->addChild(stays);
            more->addChild(alsoStays);
            more->addChild(arrives);

            mScene.clearPlacement();
            walk(*more);

            EXPECT_EQ(mScene.meshes().getRows().size(), 3u) << "the free slot was passed over and the table grew";
            EXPECT_EQ(mScene.meshes().getRows()[1].mVertices.mCount, 4u);
        }

        /// A cell that unloads takes its creatures with it, and the cell beside it keeps its own.
        ///
        /// **The shape the game makes, which `aSweepDropsWhatTheWalkNoLongerFindsAndCarriesTheRest`
        /// is not.** That one hands the walk a smaller graph; this one keeps the root and takes a
        /// child off it, because that is all `MWWorld::Scene` unloading a cell does to the picture.
        /// `MWRender::Objects` parents every reference in a cell — actors among them — to one group
        /// under the scene root, and `Objects::removeCell` takes that group off. The root never
        /// changes, so nothing announces that a cell has gone: what drops the actors that left with
        /// it is the sweep, and only the sweep.
        ///
        /// **Actors and not crates, because an actor goes on costing after it is out of reach.** A
        /// deforming drawable is a bottom-level structure rebuilt from a pose every frame, so a
        /// creature the sweep missed is not only a body standing in an unloaded town but the price
        /// of one — which is why the one that leaves is posed again after it has.
        TEST_F(RtxSceneExtractorTest, aCellTakenOffTheRootTakesItsActorsAndLeavesItsNeighboursStanding)
        {
            RiggedQuad leaves;
            RiggedQuad stays;

            // Told apart by their poses, so the survivor is recognised by what came out of it
            // rather than by being the only one left.
            leaves.mBone->setMatrix(osg::Matrix::translate(0.0, 0.0, 3.0));
            stays.mBone->setMatrix(osg::Matrix::translate(0.0, 0.0, 11.0));
            leaves.update(1);
            stays.update(1);

            // Two "Cell Root" groups, as `Objects::insertBegin` makes them: an actor and a crate in
            // the cell being walked away from, an actor in the one still under the player.
            osg::ref_ptr<osg::Group> unloading = new osg::Group;
            unloading->addChild(leaves.mSkeleton);
            unloading->addChild(makeQuad());

            osg::ref_ptr<osg::Group> loaded = new osg::Group;
            loaded->addChild(stays.mSkeleton);

            osg::ref_ptr<osg::Group> root = new osg::Group;
            root->addChild(unloading);
            root->addChild(loaded);

            // `RtxRenderer::renderFrame`'s own order, because what this guards against lives
            // between one frame and the next: clear, walk the world, close the frame, sweep.
            mScene.clearPlacement();
            const ExtractionStats arrived = mExtractor.extract(*root, osg::Matrixf::identity(), 0, 1);
            mScene.placements().advance();
            ASSERT_TRUE(mExtractor.retire().empty()) << "the walk that found them is the epoch they survive";

            EXPECT_EQ(arrived.mMeshesAdded, 3u);
            EXPECT_EQ(arrived.mDeformed, 2u);
            ASSERT_EQ(mScene.placements().getCounts().mPlaced, 3u);

            // The whole of what `Objects::removeCell` does to the graph.
            root->removeChild(unloading);

            // **Both, and the departed one first.** The game stops updating an actor whose cell has
            // gone, so posing this one is the harsher case: what decides a creature has left is
            // that the walk did not reach it, never that it stopped moving.
            leaves.update(2);
            stays.update(2);

            mScene.clearPlacement();
            const ExtractionStats after = mExtractor.extract(*root, osg::Matrixf::identity(), 0, 2);
            mScene.placements().advance();
            const Retirement went = mExtractor.retire();

            EXPECT_EQ(after.mInstances, 1u);
            EXPECT_EQ(after.mMeshesAdded, 0u) << "the cell that stayed was mirrored again rather than recognised";
            EXPECT_EQ(after.mDeformed, 1u) << "an actor out of the walk's reach was still posed for a structure";
            EXPECT_EQ(went.mMeshes, 2u) << "the crate leaves with the creature";
            EXPECT_EQ(went.mMaterials, 0u) << "an untextured quad has no state set and so no material";

            ASSERT_EQ(mScene.placements().getCounts().mPlaced, 1u);

            const auto rows = mScene.placements().getRows();
            const auto standing = std::find_if(
                rows.begin(), rows.end(), [](const PlacementRow& row) { return row.mInstance.isPlaced(); });
            ASSERT_NE(standing, rows.end());
            EXPECT_EQ(
                boneAt(mScene.getMeshPose(standing->mInstance.mMesh), 0).mRows[2], osg::Vec4f(0.0f, 0.0f, 1.0f, 11.0f))
                << "the sweep kept the actor from the cell that unloaded";
            EXPECT_EQ(mScene.deformers().getDeformers().size(), 2u) << "a rig is a slot and keeps its index";
            EXPECT_EQ(mScene.deformers().getHolds(mScene.meshes().getRows()[standing->mInstance.mMesh].mDeformer), 1u)
                << "the rig of the one that left went with it and the survivor's stayed";
        }

        /// A slot the walk stopped naming is freed on the frame it stopped, however whole the map is,
        /// and the placement that stood on it is stood again on the mesh that replaced it.
        ///
        /// **What the sweep's own guard cannot see.** A frame where every entry was reached has
        /// nothing stale in it, so the sweep and the release are both skipped — but a deforming
        /// drawable whose source geometry was replaced is not stale, it is wrong: `MeshResolver`
        /// lets go of that entry in the middle of the walk and mirrors the drawable afresh. The map
        /// ends the frame the size it started, every entry in it stamped, and the slot the abandoned
        /// entry named is named by nothing at all — the placement found under the drawable's path
        /// included, which would otherwise stand on the freed row and trace whatever is put there
        /// next.
        TEST_F(RtxSceneExtractorTest, aSlotAbandonedInsideAWalkIsFreedByTheSameFrameThatAbandonedIt)
        {
            RiggedQuad actor;
            osg::ref_ptr<osg::Geometry> crate = makeQuad();

            osg::ref_ptr<osg::Group> root = new osg::Group;
            root->addChild(actor.mSkeleton);
            root->addChild(crate);

            actor.update(1);
            mScene.clearPlacement();
            const ExtractionStats arrived = mExtractor.extract(*root, osg::Matrixf::identity(), 0, 1);
            mScene.placements().advance();
            ASSERT_TRUE(mExtractor.retire().empty()) << "the walk that found them is the epoch they survive";

            ASSERT_EQ(arrived.mMeshesAdded, 2u);
            ASSERT_EQ(mScene.meshes().getRows().size(), 2u);
            ASSERT_EQ(mScene.meshes().getRows()[0].mVertices.mCount, 4u)
                << "the actor is the first drawable under the root";

            // **The rig re-pointed at a longer mesh, which is the same rig.** Posing six vertices
            // into a run of four is not a wrong pose: the run lives in one shared vertex buffer, so
            // the kernel would write over the meshes that follow it.
            osg::ref_ptr<osg::Geometry> longer = new osg::Geometry;
            longer->setVertexArray(makePositions({
                osg::Vec3f(0.0f, 0.0f, 0.0f),
                osg::Vec3f(1.0f, 0.0f, 0.0f),
                osg::Vec3f(1.0f, 1.0f, 0.0f),
                osg::Vec3f(0.0f, 1.0f, 0.0f),
                osg::Vec3f(2.0f, 0.0f, 0.0f),
                osg::Vec3f(2.0f, 1.0f, 0.0f),
            }));
            longer->addPrimitiveSet(makeTriangles({ 0, 1, 2, 0, 2, 3, 1, 4, 5 }));
            actor.mRig->setSourceGeometry(longer);

            actor.update(2);
            mScene.clearPlacement();
            const ExtractionStats again = mExtractor.extract(*root, osg::Matrixf::identity(), 0, 2);
            mScene.placements().advance();
            const Retirement went = mExtractor.retire();

            // Both drawables were reached and the map is the size it was, so the sweep erased no
            // entry — and the slot the abandoned entry named still has to go, which is the one row
            // the release reports.
            EXPECT_EQ(again.mMeshesAdded, 1u) << "the longer mesh was posed into the slot it does not fit";
            EXPECT_EQ(again.mMeshesReused, 1u) << "the crate was mirrored again rather than recognised";
            EXPECT_EQ(again.mRestood, 1u) << "the actor's placement, found under its path on another mesh";
            EXPECT_EQ(went.mMeshes, 1u) << "the abandoned slot, and nothing the walk reached";
            EXPECT_EQ(went.mMaterials, 0u);

            ASSERT_EQ(mScene.meshes().getRows().size(), 3u);
            EXPECT_EQ(mScene.meshes().getRows()[0].mVertices.mCount, 0u) << "the abandoned slot was left standing";
            EXPECT_EQ(mScene.meshes().getRows()[1].mVertices.mCount, 4u) << "the crate lost its slot";
            EXPECT_EQ(mScene.meshes().getRows()[2].mVertices.mCount, 6u);

            // Two placements stand, and the actor's is on the longer mesh: a walk that met every
            // drawable dropped none, and what it stood again stands where the resolver answered.
            EXPECT_EQ(mScene.placements().getCounts().mPlaced, 2u);
            bool actorStands = false;
            for (const PlacementRow& row : mScene.placements().getRows())
                actorStands = actorStands || (row.mInstance.isPlaced() && row.mInstance.mMesh == 2);
            EXPECT_TRUE(actorStands) << "the actor's placement kept standing on the abandoned slot";
        }

        /// Everything under a root the caller names a class is placed as that class, and the
        /// innermost named root stands for the path.
        ///
        /// The game marks the root of an actor, an effect or the player's arms and not their
        /// drawables, so the mark is carried down the subtree: a quad under the arms' group takes
        /// `MASK_FIRST_PERSON`, one under the actor's takes `MASK_ACTOR`, one under an effect hung
        /// on that actor takes `MASK_EFFECT` — the class a camera with no `Mask_Effect` leaves out
        /// — and one beside them all, with the mask every drawable is born with, stays static.
        /// Read by the water's rule, no bit outside the named one, so the all-ones default never
        /// matches.
        TEST_F(RtxSceneExtractorTest, whatStandsUnderANamedRootIsPlacedAsItsClass)
        {
            constexpr osg::Node::NodeMask sFirstPerson = 1u << 9;
            constexpr osg::Node::NodeMask sActor = 1u << 3;
            constexpr osg::Node::NodeMask sPlayer = 1u << 4;
            constexpr osg::Node::NodeMask sEffect = 1u << 1;

            osg::ref_ptr<osg::Group> arms = new osg::Group;
            arms->setNodeMask(sFirstPerson);
            arms->addChild(makeQuad());

            osg::ref_ptr<osg::Group> effect = new osg::Group;
            effect->setNodeMask(sEffect);
            effect->addChild(makeQuad());

            osg::ref_ptr<osg::Group> actor = new osg::Group;
            actor->setNodeMask(sPlayer);
            actor->addChild(makeQuad());
            actor->addChild(effect);

            osg::ref_ptr<osg::Group> root = new osg::Group;
            root->addChild(arms);
            root->addChild(actor);
            root->addChild(makeQuad());

            mExtractor.setClassMask(Rtx::InstanceClass::FirstPerson, sFirstPerson);
            mExtractor.setClassMask(Rtx::InstanceClass::Actor, sActor | sPlayer);
            mExtractor.setClassMask(Rtx::InstanceClass::Effect, sEffect);
            walk(*root);

            std::vector<Rtx::InstanceRecord> records;
            Rtx::makeInstanceRecords(mScene, records);

            ASSERT_EQ(records.size(), 4u);
            EXPECT_EQ(records[0].mMask, Rtx::Shaders::MASK_FIRST_PERSON) << "under the arms' root";
            EXPECT_EQ(records[1].mMask, Rtx::Shaders::MASK_ACTOR) << "under the player's root";
            EXPECT_EQ(records[2].mMask, Rtx::Shaders::MASK_EFFECT) << "the effect on the player, innermost";
            EXPECT_EQ(records[3].mMask, Rtx::Shaders::MASK_STATIC) << "beside them";

            // And a caller that names no class — the harness — places the same graph as static
            // four times.
            Rtx::SceneDesc unnamed;
            SceneExtractor silent(unnamed);
            silent.extract(*root, osg::Matrixf::identity(), 0);
            Rtx::makeInstanceRecords(unnamed, records);
            ASSERT_EQ(records.size(), 4u);
            for (const Rtx::InstanceRecord& record : records)
                EXPECT_EQ(record.mMask, Rtx::Shaders::MASK_STATIC);
        }

        /// A material and the texture behind it go when the last thing wearing them does.
        TEST_F(RtxSceneExtractorTest, aSweepTakesTheMaterialsNothingWearsAndTheTexturesTheyNamed)
        {
            osg::ref_ptr<osg::Geometry> stone = makeQuad();
            paint(*stone->getOrCreateStateSet(), "textures/tx_stone_01.dds");

            walk(*stone);

            ASSERT_EQ(mScene.materials().getRows().size(), 1u);
            ASSERT_EQ(mScene.textures().getRows().size(), 1u);
            ASSERT_TRUE(mExtractor.retire().empty()) << "the walk that found it is the epoch it survives";

            // A walk that finds nothing at all is still a walk, and it is what an emptied cell is.
            osg::ref_ptr<osg::Group> nothing = new osg::Group;
            mScene.clearPlacement();
            walk(*nothing);

            const Retirement went = mExtractor.retire();
            EXPECT_EQ(went.mMeshes, 1u);
            EXPECT_EQ(went.mMaterials, 1u);

            // **Freed, not removed.** The slots stay where they are so nothing above them is
            // renumbered — there is nothing above them here, but the rule is what a cell boundary
            // depends on — and what they held is gone.
            ASSERT_EQ(mScene.meshes().getRows().size(), 1u);
            EXPECT_EQ(mScene.meshes().getRows()[0].mVertices.mCount, 0u);
            EXPECT_EQ(mScene.materials().getRows().size(), 1u);
            EXPECT_EQ(mScene.materials().getRows()[0].mDiffuse, Rtx::sNoIndex);

            // **The slot stays, and that is deliberate.** It lives in a bindless array a material
            // indexes by position, so reclaiming one renumbers the rest and the array is built again
            // — a fifth of a second, against nothing saved but a texture's bytes. What goes is what
            // was in it: the material that named it was the last thing naming it.
            EXPECT_EQ(mScene.textures().getRows().size(), 1u);
            EXPECT_TRUE(mScene.textures().getRows()[0].mPath.value().empty()) << "a texture nothing names was kept";
        }

        /// A placement found under its path wears what the walk resolved this frame, or it is
        /// dropped and stood again — and then the sweep frees what it wore before, since nothing
        /// stands on it any more.
        ///
        /// The key is a hash of node addresses nothing keeps alive, so a path whose nodes were freed
        /// and allotted again between two walks finds the entry of what stood there before; a
        /// state set the game swaps under a drawable is the same case in one node. Either way a
        /// placement that was only moved would carry the old material at the new place — and, once
        /// the sweep freed that material's row, whatever material takes the row next.
        TEST_F(RtxSceneExtractorTest, aPlacementFoundUnderItsPathIsStoodAgainWhereItResolvesToAnotherSurface)
        {
            osg::ref_ptr<osg::Geometry> stone = makeQuad();
            paint(*stone->getOrCreateStateSet(), "textures/tx_stone_01.dds");

            const ExtractionStats first = walk(*stone);
            EXPECT_EQ(first.mRestood, 0u);
            ASSERT_EQ(mScene.materials().getRows().size(), 1u);
            ASSERT_EQ(mScene.placements().getCounts().mPlaced, 1u);
            ASSERT_TRUE(mExtractor.retire().empty());

            // The same drawable on the same path, wearing another state set: the material resolver
            // answers a second row, and the placement it stood under the first has to follow.
            osg::ref_ptr<osg::StateSet> wood = new osg::StateSet;
            paint(*wood, "textures/tx_wood_01.dds");
            stone->setStateSet(wood);

            mScene.clearPlacement();
            const ExtractionStats second = walk(*stone, 0, 1);
            EXPECT_EQ(second.mRestood, 1u) << "the placement kept the material it no longer resolves to";
            EXPECT_EQ(second.mInstances, 1u);
            ASSERT_EQ(mScene.materials().getRows().size(), 2u);

            const Retirement went = mExtractor.retire();
            EXPECT_EQ(went.mMeshes, 0u) << "the quad is the quad";
            EXPECT_EQ(went.mMaterials, 1u) << "the stone, which nothing wears";

            ASSERT_EQ(mScene.placements().getCounts().mPlaced, 1u);
            for (const PlacementRow& row : mScene.placements().getRows())
            {
                if (!row.mInstance.isPlaced())
                    continue;
                EXPECT_EQ(row.mInstance.mMaterial, 1u) << "the placement stands on a material the sweep freed";
            }
            EXPECT_EQ(mScene.materials().getRows()[0].mDiffuse, Rtx::sNoIndex) << "the stone's row was kept";
        }

#ifndef NDEBUG
        /// **A walked scene is handed over after its sweep and never before it.** Between the two
        /// the scene still holds every slot the walk stopped finding, where the last frame left it,
        /// and a backend handed that traces it once more: the world's frame swept after its trace,
        /// and the player's body — stamped afresh on every cell it entered — drew as a double of
        /// itself a frame behind. The mark a walk leaves outlives a clear of the frame's lists,
        /// because clearing them settles nothing of the sweep; only the sweep does.
        TEST_F(RtxSceneExtractorTest, aWalkedSceneHandedOverBeforeItsSweepDies)
        {
            osg::ref_ptr<osg::Geometry> quad = makeQuad();

            // A scene nothing walked is handed over as it is: what a test builds by hand.
            mScene.orderLights();
            mScene.clearPlacement();

            walk(*quad);
            EXPECT_DEATH(mScene.orderLights(), "a call out of its turn");
            mScene.clearPlacement();
            EXPECT_DEATH(mScene.orderLights(), "a call out of its turn") << "a clear settled the sweep";

            ASSERT_TRUE(mExtractor.retire().empty());
            mScene.orderLights();
            mScene.orderLights();

            // And a walk on the frame after: the same again.
            mScene.clearPlacement();
            walk(*quad, 0, 1);
            EXPECT_DEATH(mScene.orderLights(), "a call out of its turn");
            ASSERT_TRUE(mExtractor.retire().empty());
            mScene.orderLights();
        }
#endif
    }
}
