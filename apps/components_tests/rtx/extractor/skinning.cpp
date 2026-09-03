#include "fixture.hpp"

#include "../allocations.hpp"

namespace Rtx::Testing
{
    namespace
    {
        /// The third row of the one bone of the quad's rig, as the scene holds it: the identity's
        /// last row with the bone's height in its last column. `RiggedQuad` binds with the identity
        /// and no skin transform, so a translated bone is exactly what the walk has to hand over.
        osg::Vec4f boneRow(const Rtx::SceneDesc& scene, Rtx::Index mesh)
        {
            return scene.getMeshBones(mesh)[0].mRows[2];
        }

        /// A skinned body is mirrored as its bind pose and its bone rows, and not as vertices.
        ///
        /// **Nothing else poses this graph**, which is the whole assertion: the rows exist only
        /// because the walk read the matrices the update traversal left in the skeleton and
        /// composed them the way `RigGeometry::cull` does. The bone is moved off the origin so that
        /// the two possible answers are two different numbers: a rig read without its skeleton
        /// carries the identity, and the pose carries it moved by five.
        TEST(RtxSceneExtractorTest, aSkinnedBodyIsMirroredAsItsBindPoseAndItsBoneRows)
        {
            RiggedQuad rigged;
            rigged.mBone->setMatrix(osg::Matrix::translate(0.0, 0.0, 5.0));
            rigged.update(1);

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);
            const ExtractionStats stats = extractor.extract(*rigged.mSkeleton, osg::Matrixf::identity(), 0);

            EXPECT_EQ(stats.mSkippedUnknown, 0u) << "a drawable that is not an osg::Geometry is still geometry";
            EXPECT_EQ(stats.mMeshesAdded, 1u);
            EXPECT_EQ(stats.mDeformed, 1u);
            EXPECT_EQ(stats.mUnskinned, 0u) << "the update found the skeleton, so the rig is skinned";
            EXPECT_EQ(stats.mInstances, 1u);
            EXPECT_EQ(scene.getTriangleCount(), 2u);

            // The mesh holds the bind pose, and the rig beside it is the quad's four one-bone runs.
            ASSERT_EQ(scene.getMeshes().size(), 1u);
            EXPECT_EQ(scene.getMeshes()[0].mDeform, Rtx::Deform::Rig);
            EXPECT_EQ(scene.getMeshPositions(0)[2], osg::Vec3f(1.0f, 1.0f, 0.0f))
                << "the bind pose, never a vertex posed";
            ASSERT_EQ(scene.getRigs().size(), 1u);
            EXPECT_EQ(scene.getRigs()[0].mBoneCount, 1u);
            EXPECT_EQ(scene.getRigs()[0].mVertexCount, 4u);
            EXPECT_EQ(scene.getRuns().size(), 4u);
            EXPECT_EQ(scene.getRuns()[3], 1u) << "first nought, count one";
            ASSERT_EQ(scene.getInfluences().size(), 1u);
            EXPECT_EQ(scene.getInfluences()[0].mBone, 0u);
            EXPECT_EQ(scene.getInfluences()[0].mWeight, 1.0f);

            // And the pose: `invBind · bone · transform` with the identity for both ends is the
            // bone itself, whose translation lands in the last column of the last row.
            ASSERT_EQ(scene.getDeformed().size(), 1u);
            EXPECT_EQ(scene.getDeformed()[0], 0u);
            EXPECT_EQ(scene.getMeshBones(0)[0].mRows[0], osg::Vec4f(1.0f, 0.0f, 0.0f, 0.0f));
            EXPECT_EQ(scene.getMeshBones(0)[0].mRows[1], osg::Vec4f(0.0f, 1.0f, 0.0f, 0.0f));
            EXPECT_EQ(boneRow(scene, 0), osg::Vec4f(0.0f, 0.0f, 1.0f, 5.0f)) << "the bind pose moved by the bone";

            // The reach is the drawable's own bound, which `updateBounds` made from the bone's
            // sphere — a sphere, and so a box wider than the quad, but one that stands at five.
            const osg::BoundingBoxf& reach = scene.getMeshes()[0].mBounds;
            EXPECT_TRUE(reach.valid());
            EXPECT_LE(reach.zMin(), 5.0f);
            EXPECT_GE(reach.zMax(), 5.0f);
        }

        /// A bone that moves between frames moves the rows, and it is the same body.
        ///
        /// The mesh is keyed on the drawable, so a second frame must not read as a second mesh —
        /// and a frame the bone stood still on must name no structure to refit.
        TEST(RtxSceneExtractorTest, aBoneThatMovesMovesTheRowsWithoutAddingAMesh)
        {
            RiggedQuad rigged;
            rigged.update(1);

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);

            extractor.extract(*rigged.mSkeleton, osg::Matrixf::identity(), 0);
            EXPECT_EQ(boneRow(scene, 0), osg::Vec4f(0.0f, 0.0f, 1.0f, 0.0f));

            rigged.mBone->setMatrix(osg::Matrix::translate(0.0, 0.0, 7.0));
            rigged.update(2);

            scene.clearPlacement();
            const ExtractionStats again = extractor.extract(*rigged.mSkeleton, osg::Matrixf::identity(), 0, 1);

            EXPECT_EQ(again.mMeshesAdded, 0u);
            EXPECT_EQ(again.mMeshesReused, 1u);
            EXPECT_EQ(again.mDeformed, 1u);
            EXPECT_EQ(scene.getMeshes().size(), 1u) << "a second pose is the same mesh";
            EXPECT_EQ(scene.getRigs().size(), 1u) << "and the same rig";
            EXPECT_EQ(boneRow(scene, 0), osg::Vec4f(0.0f, 0.0f, 1.0f, 7.0f));
            ASSERT_EQ(scene.getDeformed().size(), 1u);
            EXPECT_EQ(scene.getDeformed()[0], 0u);

            // **A frame the bone stood still on costs nothing.** The walk poses every rig it meets
            // and the scene compares the rows against the ones it holds.
            //
            // Nothing off the heap either, which is the second half of "costs nothing". A rig met
            // again is found in the skin map once and stamped through the same entry, and the count
            // that says the slot still fits is read from the arrays as they stand.
            rigged.update(3);
            scene.clearPlacement();

            const std::size_t before = Testing::getAllocationCount();
            const ExtractionStats still = extractor.extract(*rigged.mSkeleton, osg::Matrixf::identity(), 0, 2);
            const std::size_t spent = Testing::getAllocationCount() - before;

            EXPECT_EQ(spent, 0u) << spent << " allocations to pose a body the walk already held";
            EXPECT_EQ(still.mDeformed, 1u) << "posed, which is what the count says";
            EXPECT_TRUE(scene.getDeformed().empty()) << "and unchanged, which is what the list says";
            EXPECT_EQ(scene.getMeshPositions(0)[2], osg::Vec3f(1.0f, 1.0f, 0.0f)) << "and it is the same bind pose";
            EXPECT_EQ(boneRow(scene, 0), osg::Vec4f(0.0f, 0.0f, 1.0f, 7.0f)) << "held where the bone left it";

            // **And no traversal number gates it.** A walk on the same frame after the bone moved
            // again reads the matrices the update left, whatever number the walk runs at — where a
            // pose read off a cull traversal's own copy was frozen at the number it last saw.
            rigged.mBone->setMatrix(osg::Matrix::translate(0.0, 0.0, 99.0));
            rigged.update(4);

            scene.clearPlacement();
            extractor.extract(*rigged.mSkeleton, osg::Matrixf::identity(), 0, 2);
            EXPECT_EQ(boneRow(scene, 0), osg::Vec4f(0.0f, 0.0f, 1.0f, 99.0f));
        }

        /// A drawable whose geometry is not the geometry the mirror met under that address is
        /// mirrored again rather than posed into the slot the first one took.
        ///
        /// **Because the map is keyed on an address and the engine reuses them.** A body part taken
        /// off and another put on lands where the first was, so the walk that meets it finds an
        /// entry describing something else. The slot is a run inside one shared vertex buffer, and
        /// the kernel writes `mVertexCount` vertices from it: a longer mesh would run over the
        /// meshes that follow, which is not a wrong pose but a torn model — and in a release build
        /// the count is not asserted, so nothing says so. Changing the source geometry under one
        /// rig is the same fact without needing the allocator to hand back an address, and the
        /// skin rewritten in place under the same address is the same fact again for the rig.
        TEST(RtxSceneExtractorTest, aDeformingDrawableThatChangedShapeIsMirroredAgainRatherThanWrittenOver)
        {
            RiggedQuad rigged;
            rigged.update(1);

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);

            osg::ref_ptr<osg::Geometry> neighbour = makeQuad();
            osg::ref_ptr<osg::Group> root = new osg::Group;
            root->addChild(rigged.mSkeleton);
            root->addChild(neighbour);

            extractor.extract(*root, osg::Matrixf::identity(), 0);
            ASSERT_EQ(scene.getMeshes().size(), 2u);
            ASSERT_EQ(scene.getMeshPositions(0).size(), 4u);
            ASSERT_EQ(scene.getRigs().size(), 1u);

            // The quad standing next to the rig, whose vertices the overrun would land in.
            const std::vector<osg::Vec3f> before(scene.getMeshPositions(1).begin(), scene.getMeshPositions(1).end());

            // Six vertices where the slot holds four, under the same drawable and the same skin.
            osg::ref_ptr<osg::Geometry> longer = new osg::Geometry;
            longer->setVertexArray(makePositions({
                osg::Vec3f(0.0f, 0.0f, 0.0f),
                osg::Vec3f(2.0f, 0.0f, 0.0f),
                osg::Vec3f(2.0f, 2.0f, 0.0f),
                osg::Vec3f(0.0f, 2.0f, 0.0f),
                osg::Vec3f(3.0f, 0.0f, 0.0f),
                osg::Vec3f(3.0f, 3.0f, 0.0f),
            }));
            longer->addPrimitiveSet(makeTriangles({ 0, 1, 2, 0, 2, 3, 1, 4, 5 }));

            rigged.mRig->setInfluences(std::vector<SceneUtil::RigGeometry::BoneWeights>(
                6, SceneUtil::RigGeometry::BoneWeights{ { 0, 1.0f } }));
            rigged.mRig->setSourceGeometry(longer);
            rigged.update(2);

            scene.clearPlacement();
            const ExtractionStats again = extractor.extract(*root, osg::Matrixf::identity(), 0, 1);

            EXPECT_EQ(again.mMeshesAdded, 1u) << "the rig is met as something the mirror has not seen";
            EXPECT_EQ(scene.getMeshes().size(), 3u) << "and takes a slot of its own rather than the old one";
            EXPECT_EQ(scene.getRigs().size(), 2u) << "on a rig of its own, because the skin is six vertices now";
            EXPECT_EQ(scene.getRigs()[1].mVertexCount, 6u);
            EXPECT_EQ(scene.getMeshes()[2].mDeformer, 1u);

            const std::vector<osg::Vec3f> after(scene.getMeshPositions(1).begin(), scene.getMeshPositions(1).end());
            EXPECT_EQ(after, before) << "the mesh after the rig's old slot is untouched";
        }

        /// An actor the game has marked semi-active goes on animating under a walk that reaches it.
        ///
        /// **Every actor but the player is semi-active** — `MWMechanics::Actors` hands the player
        /// `Active` and everyone else `SemiActive` — and a semi-active skeleton skips its update
        /// traversal, and so stops moving its bones, once several traversals have passed with
        /// nothing reaching it. Under a renderer that culls, its cull is what keeps saying so. This
        /// walk is what says so here, and without it a street of people slides about in the pose
        /// they were in three frames after they loaded.
        TEST(RtxSceneExtractorTest, aSemiActiveSkeletonGoesOnAnimatingUnderAWalkThatReachesIt)
        {
            /// Moves the bone from inside the update traversal, which is where a keyframe
            /// controller lives.
            ///
            /// **Setting the matrix from outside would prove nothing**: the bone would move whether
            /// or not the traversal ran, and the traversal running is the entire question.
            struct BoneClock : osg::NodeCallback
            {
                void operator()(osg::Node* node, osg::NodeVisitor* nv) override
                {
                    static_cast<osg::MatrixTransform*>(node)->setMatrix(
                        osg::Matrix::translate(0.0, 0.0, static_cast<double>(nv->getTraversalNumber())));
                    traverse(node, nv);
                }
            };

            RiggedQuad rigged;
            rigged.mBone->addUpdateCallback(new BoneClock);
            rigged.mSkeleton->setActive(SceneUtil::Skeleton::SemiActive);

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);

            // Four, because the gate needs three traversals to pass before it can trip: a run of
            // two would pass with the walk saying nothing at all.
            for (unsigned int frame = 1; frame <= 4; ++frame)
            {
                rigged.update(frame);

                scene.clearPlacement();
                extractor.extract(*rigged.mSkeleton, osg::Matrixf::identity(), 0, frame - 1);

                EXPECT_EQ(boneRow(scene, 0), osg::Vec4f(0.0f, 0.0f, 1.0f, static_cast<float>(frame)))
                    << "the bone moved to " << frame << " and the mirrored pose did not follow";
            }
        }

        /// What `Inactive` actually does, and why the node mask beside it is what matters.
        ///
        /// **`Inactive` is not a defect, and this is what keeps that true.** It reads like a
        /// rasterizer decision the mirror inherited — `MWMechanics::Actors` sets it for actors
        /// outside the processing range — and the fear was that the mirror would go on reaching such
        /// an actor and find a skeleton refusing to move. It would: `SceneUtil::Skeleton::traverse`
        /// turns back an **update** visitor, so the bones stop being animated while the mirror goes
        /// on posing them, and what it hands over is the pose the last update left.
        ///
        /// What makes that harmless is the base node mask, which the same code zeroes in the same
        /// breath — so the mirror never reaches the actor at all. That is an invariant of
        /// `MWMechanics::Actors` and of nothing here, so it is written down as a test: the day the
        /// mask stops being zero, this fails and says what the consequence is.
        TEST(RtxSceneExtractorTest, anInactiveActorStopsAnimatingAndIsKeptOutOfReachByItsMask)
        {
            /// The same clock the `SemiActive` case uses: the bone moves only because the update
            /// traversal reached it, which is the entire question.
            struct BoneClock : osg::NodeCallback
            {
                void operator()(osg::Node* node, osg::NodeVisitor* nv) override
                {
                    static_cast<osg::MatrixTransform*>(node)->setMatrix(
                        osg::Matrix::translate(0.0, 0.0, static_cast<double>(nv->getTraversalNumber())));
                    traverse(node, nv);
                }
            };

            RiggedQuad rigged;
            rigged.mBone->addUpdateCallback(new BoneClock);

            // **Under a parent, because that is where a node mask is read.** OSG checks a node's
            // mask in whatever traverses *to* it, so a walk handed the masked node itself as its
            // root never asks — and a test that did would report the mask working when it was only
            // never consulted.
            osg::ref_ptr<osg::Group> world = new osg::Group;
            world->addChild(rigged.mSkeleton);

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);

            // One update while the actor is in range, so the skeleton has a pose to be frozen at.
            rigged.update(1);
            extractor.extract(*world, osg::Matrixf::identity(), 0, 0);
            ASSERT_EQ(boneRow(scene, 0), osg::Vec4f(0.0f, 0.0f, 1.0f, 1.0f));

            // Out of range. The update traversal is turned back from here on, so the bone stops at
            // one however far the clock runs — and the mirror goes on placing the actor there, and
            // names no structure to refit while it does.
            rigged.mSkeleton->setActive(SceneUtil::Skeleton::Inactive);

            for (unsigned int traversal = 2; traversal <= 4; ++traversal)
            {
                rigged.update(traversal);

                scene.clearPlacement();
                const ExtractionStats found = extractor.extract(*world, osg::Matrixf::identity(), 0, traversal - 1);

                ASSERT_EQ(found.mInstances, 1u) << "the mirror stopped reaching an Inactive actor on its own";
                EXPECT_EQ(boneRow(scene, 0), osg::Vec4f(0.0f, 0.0f, 1.0f, 1.0f))
                    << "an Inactive skeleton animated at traversal " << traversal;
                EXPECT_TRUE(scene.getDeformed().empty()) << "a pose that stood still named a structure to refit";
            }

            // And what the game does in the same breath as setting the flag, which is the half that
            // makes the frozen pose above unreachable rather than merely still.
            rigged.mSkeleton->setNodeMask(0);

            scene.clearPlacement();
            rigged.update(5);
            const ExtractionStats missed = extractor.extract(*world, osg::Matrixf::identity(), 0, 4);

            // **What the walk found, and not what the scene holds.** A slot survives
            // `clearPlacement` — that is the whole of "slots, not compaction" — and it is
            // `retire` that decides a slot nobody stood in is nobody's, on evidence this test does
            // not have: it is still holding the rig alive itself.
            EXPECT_EQ(missed.mInstances, 0u) << "an actor the game put out of reach was mirrored anyway";
            EXPECT_EQ(missed.mDeformed, 0u) << "and its pose was computed on the way past";
        }

        /// A rig no update traversal has resolved is mirrored as it stands, and counted.
        ///
        /// **The rasterizer draws such a rig in its bind pose too**: `RigGeometry::cull` finds no
        /// skeleton and returns before it skins. So this is what the game shows and not a loss — and
        /// the count is what says a walk ran before the update it depends on.
        TEST(RtxSceneExtractorTest, aRigWhoseSkeletonNoUpdateFoundIsMirroredAsItStands)
        {
            RiggedQuad rigged;
            rigged.mBone->setMatrix(osg::Matrix::translate(0.0, 0.0, 5.0));

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);
            const ExtractionStats stats = extractor.extract(*rigged.mSkeleton, osg::Matrixf::identity(), 0);

            EXPECT_EQ(stats.mMeshesAdded, 1u);
            EXPECT_EQ(stats.mDeformed, 0u);
            EXPECT_EQ(stats.mUnskinned, 1u);
            ASSERT_EQ(scene.getMeshes().size(), 1u);
            EXPECT_EQ(scene.getMeshes()[0].mDeform, Rtx::Deform::None);
            EXPECT_TRUE(scene.getRigs().empty());
            EXPECT_EQ(scene.getMeshPositions(0)[2], osg::Vec3f(1.0f, 1.0f, 0.0f)) << "the bind pose, where it stands";
        }

        /// A morphed face is mirrored as its base and its weights, and posed again each pass — and
        /// a static drawable met again is not.
        ///
        /// The two kinds stand side by side in one graph and one pass, so the same run produces both
        /// answers and the difference cannot be a difference in how they were set up.
        TEST(RtxSceneExtractorTest, aMorphedFaceIsPosedAgainEachPassAndAStaticDrawableIsNot)
        {
            osg::ref_ptr<SceneUtil::MorphGeometry> morph = new SceneUtil::MorphGeometry;
            morph->setSourceGeometry(makeQuad());

            // The base target is what the pose starts from; the second is added at its weight. One
            // unit of z per unit of weight, so what the device would compute is arithmetic rather
            // than a fit.
            morph->addMorphTarget(makePositions({
                osg::Vec3f(0.0f, 0.0f, 0.0f),
                osg::Vec3f(1.0f, 0.0f, 0.0f),
                osg::Vec3f(1.0f, 1.0f, 0.0f),
                osg::Vec3f(0.0f, 1.0f, 0.0f),
            }));
            morph->addMorphTarget(makePositions({
                osg::Vec3f(0.0f, 0.0f, 1.0f),
                osg::Vec3f(0.0f, 0.0f, 1.0f),
                osg::Vec3f(0.0f, 0.0f, 1.0f),
                osg::Vec3f(0.0f, 0.0f, 1.0f),
            }));

            osg::ref_ptr<osg::Group> root = new osg::Group;
            root->addChild(makeQuad());
            root->addChild(morph);

            constexpr Rtx::Index sStill = 0;
            constexpr Rtx::Index sFace = 1;

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);

            const ExtractionStats first = extractor.extract(*root, osg::Matrixf::identity(), 0);
            EXPECT_EQ(first.mMeshesAdded, 2u);
            EXPECT_EQ(first.mDeformed, 1u);
            EXPECT_EQ(scene.getMeshes()[sFace].mDeform, Rtx::Deform::Morph);
            EXPECT_EQ(scene.getMeshPositions(sFace)[2], osg::Vec3f(1.0f, 1.0f, 0.0f)) << "the base, and never a pose";

            // The offsets, target by target: the base's four zeroes and then the unit lift.
            ASSERT_EQ(scene.getMorphs().size(), 1u);
            EXPECT_EQ(scene.getMorphs()[0].mTargetCount, 2u);
            ASSERT_EQ(scene.getMorphOffsets().size(), 8u);
            EXPECT_EQ(scene.getMorphOffsets()[2], osg::Vec3f());
            EXPECT_EQ(scene.getMorphOffsets()[6], osg::Vec3f(0.0f, 0.0f, 1.0f));

            // The weights as the drawable numbers them, the base's carried and never read.
            ASSERT_EQ(scene.getMeshWeights(sFace).size(), 2u);
            EXPECT_EQ(scene.getMeshWeights(sFace)[1], 1.0f);

            scene.clearPlacement();
            morph->getMorphTarget(1).setWeight(3.0f);
            morph->dirty();

            const ExtractionStats second = extractor.extract(*root, osg::Matrixf::identity(), 0);

            EXPECT_EQ(second.mMeshesAdded, 0u);
            EXPECT_EQ(second.mMeshesReused, 2u);
            EXPECT_EQ(second.mDeformed, 1u) << "the face, and only the face";
            EXPECT_EQ(scene.getMeshes().size(), 2u);

            ASSERT_EQ(scene.getDeformed().size(), 1u);
            EXPECT_EQ(scene.getDeformed()[0], sFace) << "the still quad's structure is not refitted";
            EXPECT_EQ(scene.getMeshWeights(sFace)[1], 3.0f);
            EXPECT_EQ(scene.getMeshPositions(sStill)[2], osg::Vec3f(1.0f, 1.0f, 0.0f)) << "and the neighbour is intact";

            // **A third walk, which is the one that must reach the heap not at all.** A morphed face
            // is found in the target map once and stamped through that same entry, and the base it
            // is measured against is read from the targets as they stand.
            scene.clearPlacement();

            const std::size_t before = Testing::getAllocationCount();
            const ExtractionStats third = extractor.extract(*root, osg::Matrixf::identity(), 0);
            const std::size_t spent = Testing::getAllocationCount() - before;

            EXPECT_EQ(spent, 0u) << spent << " allocations to pose a face the walk already held";
            EXPECT_EQ(third.mMeshesReused, 2u);
            EXPECT_EQ(scene.getMeshWeights(sFace)[1], 3.0f) << "at the same weight";
            EXPECT_EQ(scene.getMeshPositions(sFace)[2], osg::Vec3f(1.0f, 1.0f, 0.0f)) << "off the same base";
            EXPECT_TRUE(scene.getDeformed().empty()) << "a pose that stood still named a structure to refit";
        }

        /// Shading that exists only inside a cull traversal is applied by the walk and read from it.
        ///
        /// **This is what a fire is.** `NifOsg` hangs the controllers of anything marked
        /// `AnimFlag_AutoPlay` from a cull callback, and `SceneUtil::StateSetUpdater` as a cull
        /// callback writes into a state set it keys on the visitor and pushes onto that visitor's
        /// stack — never onto the node. A mirror that walked outside a cull would find the node
        /// bare and draw the frame it first met for ever.
        ///
        /// The slot is the second half of it: the surface has not moved and its material must not
        /// be dropped and added again to say so.
        TEST(RtxSceneExtractorTest, shadingOnlyACullTraversalSeesIsAppliedAndReadAgainEachFrame)
        {
            osg::ref_ptr<ColourController> controller = new ColourController;
            controller->mRed = 0.25f;

            osg::ref_ptr<osg::Group> node = new osg::Group;
            node->addChild(makeQuad());
            node->addCullCallback(controller);

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);

            const ExtractionStats first = extractor.extract(*node, osg::Matrixf::identity(), 0);
            ASSERT_EQ(first.mMaterialsAdded, 1u) << "the controller's state set is the only one on the path";
            ASSERT_EQ(scene.getMaterials().size(), 1u);
            EXPECT_EQ(scene.getMaterials()[0].mDiffuseColour, osg::Vec4f(0.25f, 0.0f, 0.0f, 1.0f));

            controller->mRed = 0.75f;
            scene.clearPlacement();
            const ExtractionStats second = extractor.extract(*node, osg::Matrixf::identity(), 0, 1);

            EXPECT_EQ(second.mMaterialsAdded, 0u) << "the surface did not change, only what it is wearing";
            EXPECT_EQ(second.mMaterialsReused, 1u);
            ASSERT_EQ(scene.getMaterials().size(), 1u);
            EXPECT_EQ(scene.getMaterials()[0].mDiffuseColour, osg::Vec4f(0.75f, 0.0f, 0.0f, 1.0f));

            // And a frame the controller said nothing new on writes nothing to the device: the row
            // goes over when the scene names it, and only then.
            scene.clearArrivals();
            scene.clearPlacement();
            extractor.extract(*node, osg::Matrixf::identity(), 0, 2);
            EXPECT_TRUE(scene.getWrittenMaterials().empty()) << "re-reading an unchanged state set is not a change";
        }
    }
}
