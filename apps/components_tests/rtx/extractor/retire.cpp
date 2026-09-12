#include "fixture.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <components/rtx/material.hpp>
#include <components/rtx/meshinstance.hpp>
#include <components/rtx/meshrange.hpp>

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
            ASSERT_EQ(mScene.getTables().mMeshes.getRows().size(), 1u);

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
            ASSERT_EQ(mScene.getTables().mMeshes.getRows().size(), 2u);
            EXPECT_EQ(mScene.getTables().mMeshes.getMeshPositions(1)[0].z(), 5.0f);

            // **And the sweep is what lets go.** Holding the key is what costs: geometry the graph
            // dropped outlives its owner until here, and a caller that never sweeps holds every
            // drawable it has ever walked.
            const Retirement went = mExtractor.retire();
            EXPECT_EQ(went.mMeshes, 1u);
            EXPECT_FALSE(watch.valid()) << "the sweep dropped the entry and kept the drawable alive";
        }

        /// A residency standing a mesh and a material of its own, which no drawable names.
        ///
        /// What the cell ring does for a cell's ground: the rows are added straight to the scene,
        /// held on it, and let go of by giving the holds back — outside any walk, which is when a
        /// detached world lets go of everything.
        class OwnedRows : public Residency
        {
        public:
            explicit OwnedRows(SceneDesc& scene)
                : mScene(scene)
            {
            }

            void follow(const WorldAround&) override {}

            void letGo()
            {
                mScene.placements().drop(mSlot);
                mSlot = sNoIndex;
                mScene.meshes().drop(mMesh);
                mScene.materials().drop(mMaterial);
            }

            Index getMesh() const { return mMesh; }

            void collect(SceneAdopter&, ExtractionStats& stats) override
            {
                if (mMesh != sNoIndex)
                    return;

                const std::array<osg::Vec3f, 3> corners{ osg::Vec3f(0.0f, 0.0f, 0.0f), osg::Vec3f(1.0f, 0.0f, 0.0f),
                    osg::Vec3f(0.0f, 1.0f, 0.0f) };
                const std::array<std::uint32_t, 3> triangle{ 0, 1, 2 };

                mMaterial = mScene.materials().add(Material{ .mKind = MaterialKind::Terrain });
                mMesh = mScene.addMesh(MeshArrays{ .mPositions = corners, .mIndices = triangle }, FoldedShape{},
                    Deform::None, sNoIndex, mMaterial);
                mSlot = mScene.addInstance(MeshInstance{ .mMesh = mMesh, .mMaterial = mMaterial });
                mScene.meshes().hold(mMesh);
                mScene.materials().hold(mMaterial);

                ++stats.mMeshesAdded;
                ++stats.mMaterialsAdded;
            }

        private:
            SceneDesc& mScene;
            Index mMesh = sNoIndex;
            Index mMaterial = sNoIndex;
            Index mSlot = sNoIndex;
        };

        /// **A row a residency holds survives every sweep, and goes on the first after the hold is
        /// given back.** The identity maps hold nothing for it, so without the hold the sweep after
        /// the first walk would release the ground under the player's feet — and without the scene
        /// saying a hold went, a sweep on a frame where every map stood whole would never run at
        /// all.
        TEST_F(RtxSceneExtractorTest, aRowAResidencyHoldsIsKeptWhileHeldAndReleasedWhenLetGo)
        {
            OwnedRows rows(mScene);
            Residency* held = &rows;
            mExtractor.follow(std::span<Residency* const>(&held, 1));

            osg::ref_ptr<osg::Group> nothing = new osg::Group;
            const ExtractionStats first = mExtractor.extractWorld(*nothing, osg::Matrixf::identity(), 0, 1);
            EXPECT_EQ(first.mMeshesAdded, 1u);
            EXPECT_EQ(first.mMaterialsAdded, 1u);
            ASSERT_EQ(mScene.getTables().mMeshes.getLiveCount(), 1u);

            EXPECT_TRUE(mExtractor.retire().empty()) << "a held row is a survivor";
            EXPECT_EQ(mScene.getTables().mMeshes.getLiveCount(), 1u);

            // A second walk with nothing else in it: every map stands whole, and the row still
            // stands.
            mExtractor.extractWorld(*nothing, osg::Matrixf::identity(), 0, 2);
            EXPECT_TRUE(mExtractor.retire().empty());
            EXPECT_EQ(mScene.getTables().mMeshes.getLiveCount(), 1u);

            // Let go of between walks, as a detached world does, and gone on the sweep after the
            // next — whose maps stand whole, so it is the dropped hold alone that runs it.
            rows.letGo();
            EXPECT_TRUE(mScene.hasDroppedHolds());
            mExtractor.extractWorld(*nothing, osg::Matrixf::identity(), 0, 3);

            const Retirement went = mExtractor.retire();
            EXPECT_EQ(went.mMeshes, 1u);
            EXPECT_EQ(went.mMaterials, 1u);
            EXPECT_EQ(mScene.getTables().mMeshes.getLiveCount(), 0u) << "the row nothing holds was released";
            EXPECT_EQ(mScene.getTables().mMaterials.getLiveCount(), 0u);
            EXPECT_FALSE(mScene.hasDroppedHolds());
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
            ASSERT_EQ(mScene.getTables().mMeshes.getRows().size(), 3u);

            // Nothing has gone yet, so the sweep is a no-op — and the epoch it opens is what the
            // next walk is measured against.
            EXPECT_TRUE(mExtractor.retire().empty());
            EXPECT_EQ(mScene.getTables().mMeshes.getRows().size(), 3u);

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
            ASSERT_EQ(mScene.getTables().mMeshes.getRows().size(), 3u);
            EXPECT_EQ(mScene.getTables().mMeshes.getRows()[1].mVertices.mCount, 0u) << "the middle slot should be free";
            EXPECT_EQ(mScene.getTables().mMeshes.getMeshPositions(2)[0].z(), 7.0f) << "a survivor moved";

            mScene.clearPlacement();
            const ExtractionStats after = walk(*less);

            EXPECT_EQ(after.mMeshesAdded, 0u) << "a survivor was re-added rather than recognised";
            EXPECT_EQ(after.mMeshesReused, 2u);

            // **Five slots and two standing in them.** The three walked under `whole` are gone —
            // that graph is not walked any more, so the sweep took their placements — and the two
            // walked under `less` are different placements of the same geometry, so they took slots
            // of their own. A dropped placement leaves its slot behind rather than closing the gap.
            ASSERT_EQ(mScene.getTables().mPlacements.getPlacedCount(), 2u);
            ASSERT_EQ(mScene.getTables().mPlacements.getAll().size(), 5u);
            for (std::size_t gap = 0; gap < 3; ++gap)
                EXPECT_FALSE(mScene.getTables().mPlacements.getAll()[gap].isPlaced())
                    << "slot " << gap << " should be a gap";

            // And what those placements name is what they always named, because nothing was carried
            // anywhere: the third quad is still mesh two, where it was put.
            ASSERT_TRUE(mScene.getTables().mPlacements.getAll()[3].isPlaced());
            ASSERT_TRUE(mScene.getTables().mPlacements.getAll()[4].isPlaced());
            EXPECT_EQ(mScene.getTables().mPlacements.getAll()[3].mMesh, 0u);
            EXPECT_EQ(mScene.getTables().mPlacements.getAll()[4].mMesh, 2u);

            // The freed slot goes to the next quad that turns up, which is the same size as the one
            // that left it.
            osg::ref_ptr<osg::Geometry> arrives = makeQuad();
            osg::ref_ptr<osg::Group> more = new osg::Group;
            more->addChild(stays);
            more->addChild(alsoStays);
            more->addChild(arrives);

            mScene.clearPlacement();
            walk(*more);

            EXPECT_EQ(mScene.getTables().mMeshes.getRows().size(), 3u)
                << "the free slot was passed over and the table grew";
            EXPECT_EQ(mScene.getTables().mMeshes.getRows()[1].mVertices.mCount, 4u);
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
            const ExtractionStats arrived = mExtractor.extractWorld(*root, osg::Matrixf::identity(), 0, 1);
            mExtractor.advance();
            ASSERT_TRUE(mExtractor.retire().empty()) << "the walk that found them is the epoch they survive";

            EXPECT_EQ(arrived.mMeshesAdded, 3u);
            EXPECT_EQ(arrived.mDeformed, 2u);
            ASSERT_EQ(mScene.getTables().mPlacements.getPlacedCount(), 3u);

            // The whole of what `Objects::removeCell` does to the graph.
            root->removeChild(unloading);

            // **Both, and the departed one first.** The game stops updating an actor whose cell has
            // gone, so posing this one is the harsher case: what decides a creature has left is
            // that the walk did not reach it, never that it stopped moving.
            leaves.update(2);
            stays.update(2);

            mScene.clearPlacement();
            const ExtractionStats after = mExtractor.extractWorld(*root, osg::Matrixf::identity(), 0, 2);
            mExtractor.advance();
            const Retirement went = mExtractor.retire();

            EXPECT_EQ(after.mInstances, 1u);
            EXPECT_EQ(after.mMeshesAdded, 0u) << "the cell that stayed was mirrored again rather than recognised";
            EXPECT_EQ(after.mDeformed, 1u) << "an actor out of the walk's reach was still posed for a structure";
            EXPECT_EQ(went.mMeshes, 2u) << "the crate leaves with the creature";
            EXPECT_EQ(went.mMaterials, 0u) << "an untextured quad has no state set and so no material";

            ASSERT_EQ(mScene.getTables().mPlacements.getPlacedCount(), 1u);

            const auto instances = mScene.getTables().mPlacements.getAll();
            const auto standing = std::find_if(
                instances.begin(), instances.end(), [](const MeshInstance& slot) { return slot.isPlaced(); });
            ASSERT_NE(standing, instances.end());
            EXPECT_EQ(mScene.getTables().getMeshBones(standing->mMesh)[0].mRows[2], osg::Vec4f(0.0f, 0.0f, 1.0f, 11.0f))
                << "the sweep kept the actor from the cell that unloaded";
            EXPECT_EQ(mScene.getTables().mDeformers.getRigs().size(), 2u) << "a rig is a slot and keeps its index";
            EXPECT_EQ(mScene.getTables().mDeformers.getRigHolds(
                          mScene.getTables().mMeshes.getRows()[standing->mMesh].mDeformer),
                1u)
                << "the rig of the one that left went with it and the survivor's stayed";
        }

        /// A slot the walk stopped naming is freed on the frame it stopped, however whole the map is.
        ///
        /// **What the sweep's own guard cannot see.** A frame where every entry was reached has
        /// nothing stale in it, so the sweep and the release are both skipped — but a deforming
        /// drawable whose source geometry was replaced is not stale, it is wrong: `MeshResolver`
        /// lets go of that entry in the middle of the walk and mirrors the drawable afresh. The map
        /// ends the frame the size it started, every entry in it stamped, and the slot the abandoned
        /// entry named is named by nothing at all.
        TEST_F(RtxSceneExtractorTest, aSlotAbandonedInsideAWalkIsFreedByTheSameFrameThatAbandonedIt)
        {
            RiggedQuad actor;
            osg::ref_ptr<osg::Geometry> crate = makeQuad();

            osg::ref_ptr<osg::Group> root = new osg::Group;
            root->addChild(actor.mSkeleton);
            root->addChild(crate);

            actor.update(1);
            mScene.clearPlacement();
            const ExtractionStats arrived = mExtractor.extractWorld(*root, osg::Matrixf::identity(), 0, 1);
            mExtractor.advance();
            ASSERT_TRUE(mExtractor.retire().empty()) << "the walk that found them is the epoch they survive";

            ASSERT_EQ(arrived.mMeshesAdded, 2u);
            ASSERT_EQ(mScene.getTables().mMeshes.getRows().size(), 2u);
            ASSERT_EQ(mScene.getTables().mMeshes.getRows()[0].mVertices.mCount, 4u)
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
            const ExtractionStats again = mExtractor.extractWorld(*root, osg::Matrixf::identity(), 0, 2);
            mExtractor.advance();
            const Retirement went = mExtractor.retire();

            // Both drawables were reached and the map is the size it was, so the sweep erased no
            // entry — and the slot the abandoned entry named still has to go, which is the one row
            // the release reports.
            EXPECT_EQ(again.mMeshesAdded, 1u) << "the longer mesh was posed into the slot it does not fit";
            EXPECT_EQ(again.mMeshesReused, 1u) << "the crate was mirrored again rather than recognised";
            EXPECT_EQ(went.mMeshes, 1u) << "the abandoned slot, and nothing the walk reached";
            EXPECT_EQ(went.mMaterials, 0u);

            ASSERT_EQ(mScene.getTables().mMeshes.getRows().size(), 3u);
            EXPECT_EQ(mScene.getTables().mMeshes.getRows()[0].mVertices.mCount, 0u)
                << "the abandoned slot was left standing";
            EXPECT_EQ(mScene.getTables().mMeshes.getRows()[1].mVertices.mCount, 4u) << "the crate lost its slot";
            EXPECT_EQ(mScene.getTables().mMeshes.getRows()[2].mVertices.mCount, 6u);
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
            Rtx::makeInstanceRecords(mScene.getTables(), records);

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
            Rtx::makeInstanceRecords(unnamed.getTables(), records);
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

            ASSERT_EQ(mScene.getTables().mMaterials.getRows().size(), 1u);
            ASSERT_EQ(mScene.getTables().mTextures.getPaths().size(), 1u);
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
            ASSERT_EQ(mScene.getTables().mMeshes.getRows().size(), 1u);
            EXPECT_EQ(mScene.getTables().mMeshes.getRows()[0].mVertices.mCount, 0u);
            EXPECT_EQ(mScene.getTables().mMaterials.getRows().size(), 1u);
            EXPECT_EQ(mScene.getTables().mMaterials.getRows()[0].mDiffuse, Rtx::sNoIndex);

            // **The slot stays, and that is deliberate.** It lives in a bindless array a material
            // indexes by position, so reclaiming one renumbers the rest and the array is built again
            // — a fifth of a second, against nothing saved but a texture's bytes. What goes is what
            // was in it: the material that named it was the last thing naming it.
            EXPECT_EQ(mScene.getTables().mTextures.getPaths().size(), 1u);
            EXPECT_TRUE(mScene.getTables().mTextures.getPaths()[0].value().empty())
                << "a texture nothing names was kept";
        }
    }
}
