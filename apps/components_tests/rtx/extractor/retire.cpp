#include "fixture.hpp"

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
        /// A drawable the graph has let go cannot be mistaken for whatever replaces it.
        ///
        /// **The torn figure a change of clothes produced.** `NpcAnimation::updateParts` frees the
        /// body parts that changed and builds their replacements, and the allocator is free to put a
        /// new part exactly where a retired one was; a map keyed on the bare address then finds the
        /// retired part's entry under the new part's and mirrors geometry that has nothing to do with
        /// it. The entry owns its subject, so that address is not available to hand out again until
        /// the sweep lets go — which is what makes the identity true rather than likely.
        TEST(RtxSceneExtractorTest, aDrawableTheGraphLetGoKeepsItsAddressUntilTheSweepReleasesIt)
        {
            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);

            osg::ref_ptr<osg::Group> root = new osg::Group;
            osg::ref_ptr<osg::Geometry> part = makeQuad();
            root->addChild(part);

            extractor.extract(*root, osg::Matrixf::identity(), 0);
            ASSERT_EQ(scene.getMeshes().size(), 1u);

            // The epoch this opens is what the walk below is measured against, so the sweep at the
            // end has something to find stale.
            ASSERT_TRUE(extractor.retire().empty());

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
            scene.clearPlacement();

            const ExtractionStats again = extractor.extract(*root, osg::Matrixf::identity(), 0, 1);
            EXPECT_EQ(again.mMeshesAdded, 1u) << "the replacement resolved to the retired part's mesh";
            EXPECT_EQ(again.mMeshesReused, 0u);

            // Two slots, and the new one carries its own vertices rather than the retired one's.
            ASSERT_EQ(scene.getMeshes().size(), 2u);
            EXPECT_EQ(scene.getMeshPositions(1)[0].z(), 5.0f);

            // **And the sweep is what lets go.** Holding the key is what costs: geometry the graph
            // dropped outlives its owner until here, and a caller that never sweeps holds every
            // drawable it has ever walked.
            const Retirement went = extractor.retire();
            EXPECT_EQ(went.mMeshes, 1u);
            EXPECT_FALSE(watch.valid()) << "the sweep dropped the entry and kept the drawable alive";
        }

        TEST(RtxSceneExtractorTest, aSweepDropsWhatTheWalkNoLongerFindsAndCarriesTheRest)
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

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);
            extractor.extract(*whole, osg::Matrixf::identity(), 0);
            ASSERT_EQ(scene.getMeshes().size(), 3u);

            // Nothing has gone yet, so the sweep is a no-op — and the epoch it opens is what the
            // next walk is measured against.
            EXPECT_TRUE(extractor.retire().empty());
            EXPECT_EQ(scene.getMeshes().size(), 3u);

            osg::ref_ptr<osg::Group> less = new osg::Group;
            less->addChild(stays);
            less->addChild(alsoStays);

            scene.clearPlacement();
            extractor.extract(*less, osg::Matrixf::identity(), 0);

            const Retirement went = extractor.retire();
            EXPECT_EQ(went.mMeshes, 1u);
            EXPECT_EQ(went.mMaterials, 0u) << "an untextured quad has no state set and so no material";

            // **The table is the same size and the survivors are where they were.** Freeing a slot
            // in place is what lets a cell leave without renumbering every mesh in the world, and
            // renumbering is what made a boundary cost a full rebuild.
            ASSERT_EQ(scene.getMeshes().size(), 3u);
            EXPECT_EQ(scene.getMeshes()[1].mVertexCount, 0u) << "the middle slot should be free";
            EXPECT_EQ(scene.getMeshPositions(2)[0].z(), 7.0f) << "a survivor moved";

            scene.clearPlacement();
            const ExtractionStats after = extractor.extract(*less, osg::Matrixf::identity(), 0);

            EXPECT_EQ(after.mMeshesAdded, 0u) << "a survivor was re-added rather than recognised";
            EXPECT_EQ(after.mMeshesReused, 2u);

            // **Five slots and two standing in them.** The three walked under `whole` are gone —
            // that graph is not walked any more, so the sweep took their placements — and the two
            // walked under `less` are different placements of the same geometry, so they took slots
            // of their own. A dropped placement leaves its slot behind rather than closing the gap.
            ASSERT_EQ(scene.getPlacedCount(), 2u);
            ASSERT_EQ(scene.getInstances().size(), 5u);
            for (std::size_t gap = 0; gap < 3; ++gap)
                EXPECT_FALSE(scene.getInstances()[gap].isPlaced()) << "slot " << gap << " should be a gap";

            // And what those placements name is what they always named, because nothing was carried
            // anywhere: the third quad is still mesh two, where it was put.
            ASSERT_TRUE(scene.getInstances()[3].isPlaced());
            ASSERT_TRUE(scene.getInstances()[4].isPlaced());
            EXPECT_EQ(scene.getInstances()[3].mMesh, 0u);
            EXPECT_EQ(scene.getInstances()[4].mMesh, 2u);

            // The freed slot goes to the next quad that turns up, which is the same size as the one
            // that left it.
            osg::ref_ptr<osg::Geometry> arrives = makeQuad();
            osg::ref_ptr<osg::Group> more = new osg::Group;
            more->addChild(stays);
            more->addChild(alsoStays);
            more->addChild(arrives);

            scene.clearPlacement();
            extractor.extract(*more, osg::Matrixf::identity(), 0);

            EXPECT_EQ(scene.getMeshes().size(), 3u) << "the free slot was passed over and the table grew";
            EXPECT_EQ(scene.getMeshes()[1].mVertexCount, 4u);
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
        TEST(RtxSceneExtractorTest, aCellTakenOffTheRootTakesItsActorsAndLeavesItsNeighboursStanding)
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

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);

            // `RtxRenderer::renderFrame`'s own order, because what this guards against lives
            // between one frame and the next: clear, walk the world, close the frame, sweep.
            scene.clearPlacement();
            const ExtractionStats arrived = extractor.extractWorld(*root, osg::Matrixf::identity(), 0, 1);
            extractor.advance();
            ASSERT_TRUE(extractor.retire().empty()) << "the walk that found them is the epoch they survive";

            EXPECT_EQ(arrived.mMeshesAdded, 3u);
            EXPECT_EQ(arrived.mDeformed, 2u);
            ASSERT_EQ(scene.getPlacedCount(), 3u);

            // The whole of what `Objects::removeCell` does to the graph.
            root->removeChild(unloading);

            // **Both, and the departed one first.** The game stops updating an actor whose cell has
            // gone, so posing this one is the harsher case: what decides a creature has left is
            // that the walk did not reach it, never that it stopped moving.
            leaves.update(2);
            stays.update(2);

            scene.clearPlacement();
            const ExtractionStats after = extractor.extractWorld(*root, osg::Matrixf::identity(), 0, 2);
            extractor.advance();
            const Retirement went = extractor.retire();

            EXPECT_EQ(after.mInstances, 1u);
            EXPECT_EQ(after.mMeshesAdded, 0u) << "the cell that stayed was mirrored again rather than recognised";
            EXPECT_EQ(after.mDeformed, 1u) << "an actor out of the walk's reach was still posed for a structure";
            EXPECT_EQ(went.mMeshes, 2u) << "the crate leaves with the creature";
            EXPECT_EQ(went.mMaterials, 0u) << "an untextured quad has no state set and so no material";

            ASSERT_EQ(scene.getPlacedCount(), 1u);

            const auto instances = scene.getInstances();
            const auto standing = std::find_if(
                instances.begin(), instances.end(), [](const MeshInstance& slot) { return slot.isPlaced(); });
            ASSERT_NE(standing, instances.end());
            EXPECT_EQ(scene.getMeshPositions(standing->mMesh)[2], osg::Vec3f(1.0f, 1.0f, 11.0f))
                << "the sweep kept the actor from the cell that unloaded";
        }

        /// The sea is named by a node mask, and only the drawables that carry it become water.
        ///
        /// **The engine is the only thing that knows.** Water reaches the mirror as a blended quad
        /// with a texture on it and nothing else — no geometry, state set or name tells it apart
        /// from a painted floor — so `MWRender::Water`'s own node mask is the answer, and a mirror
        /// that is not told keeps every surface a surface.
        /// Everything under the node the caller calls first person is placed for the eye alone.
        ///
        /// The game marks the root of the player's arms and not their drawables, so the mark is
        /// carried down the subtree: a quad under the marked group takes `MASK_FIRST_PERSON`, and
        /// one beside it — with the mask every drawable is born with — stays solid. Read by the
        /// water's rule, no bit outside the named one, so the all-ones default never matches.
        TEST(RtxSceneExtractorTest, whatStandsUnderTheFirstPersonRootIsPlacedForTheEyeAlone)
        {
            constexpr osg::Node::NodeMask sFirstPerson = 1u << 9;

            osg::ref_ptr<osg::Group> arms = new osg::Group;
            arms->setNodeMask(sFirstPerson);
            arms->addChild(makeQuad());

            osg::ref_ptr<osg::Group> root = new osg::Group;
            root->addChild(arms);
            root->addChild(makeQuad());

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);
            extractor.setFirstPersonMask(sFirstPerson);
            extractor.extract(*root, osg::Matrixf::identity(), 0);

            std::vector<Rtx::InstanceRecord> records;
            Rtx::makeInstanceRecords(scene, records);

            ASSERT_EQ(records.size(), 2u);
            EXPECT_EQ(records[0].mMask, Rtx::Shaders::MASK_FIRST_PERSON) << "under the arms' root";
            EXPECT_EQ(records[1].mMask, Rtx::Shaders::MASK_SOLID) << "beside it";

            // And a caller that names no mask — the harness — places the same graph as solid twice.
            Rtx::SceneDesc unnamed;
            SceneExtractor silent(unnamed);
            silent.extract(*root, osg::Matrixf::identity(), 0);
            Rtx::makeInstanceRecords(unnamed, records);
            EXPECT_EQ(records[0].mMask, Rtx::Shaders::MASK_SOLID);
        }

        /// A material and the texture behind it go when the last thing wearing them does.
        TEST(RtxSceneExtractorTest, aSweepTakesTheMaterialsNothingWearsAndTheTexturesTheyNamed)
        {
            osg::ref_ptr<osg::Geometry> stone = makeQuad();
            paint(*stone->getOrCreateStateSet(), "textures/tx_stone_01.dds");

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);
            extractor.extract(*stone, osg::Matrixf::identity(), 0);

            ASSERT_EQ(scene.getMaterials().size(), 1u);
            ASSERT_EQ(scene.getTextures().size(), 1u);
            ASSERT_TRUE(extractor.retire().empty()) << "the walk that found it is the epoch it survives";

            // A walk that finds nothing at all is still a walk, and it is what an emptied cell is.
            osg::ref_ptr<osg::Group> nothing = new osg::Group;
            scene.clearPlacement();
            extractor.extract(*nothing, osg::Matrixf::identity(), 0);

            const Retirement went = extractor.retire();
            EXPECT_EQ(went.mMeshes, 1u);
            EXPECT_EQ(went.mMaterials, 1u);

            // **Freed, not removed.** The slots stay where they are so nothing above them is
            // renumbered — there is nothing above them here, but the rule is what a cell boundary
            // depends on — and what they held is gone.
            ASSERT_EQ(scene.getMeshes().size(), 1u);
            EXPECT_EQ(scene.getMeshes()[0].mVertexCount, 0u);
            EXPECT_EQ(scene.getMaterials().size(), 1u);
            EXPECT_EQ(scene.getMaterials()[0].mDiffuse, Rtx::sNoIndex);

            // **The slot stays, and that is deliberate.** It lives in a bindless array a material
            // indexes by position, so reclaiming one renumbers the rest and the array is built again
            // — a fifth of a second, against nothing saved but a texture's bytes. What goes is what
            // was in it: the material that named it was the last thing naming it.
            EXPECT_EQ(scene.getTextures().size(), 1u);
            EXPECT_TRUE(scene.getTextures()[0].value().empty()) << "a texture nothing names was kept";
        }
    }
}
