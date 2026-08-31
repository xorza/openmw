#include "fixture.hpp"

namespace Rtx::Testing
{
    namespace
    {
        /// Runs a deforming drawable's own cull path, which is where its vertices are computed.
        ///
        /// An `osgUtil::CullVisitor` proper would need a render stage and a state graph behind it.
        /// What `MorphGeometry` actually reads of the visitor is its traversal number and the node
        /// path, so one that merely says it is a cull drives the real deformation — which is the
        /// point: this test asserts against vertices the production code computed, not against a
        /// stand-in for them.
        struct DeformingCull : osg::NodeVisitor
        {
            DeformingCull()
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            {
                setVisitorType(CULL_VISITOR);
            }
        };

        /// A skinned body is mirrored from the pose the walk itself computed.
        ///
        /// **Nothing else culls this graph**, which is the whole assertion: the pose exists only
        /// because the mirror's own traversal is a cull traversal and skinned it. Under a plain
        /// visitor `RigGeometry::accept` skins nothing and hands back whatever the last cull left,
        /// so an actor nobody had drawn yet would arrive in its bind pose and one who had walked
        /// off screen would stay in the pose they were last seen in.
        ///
        /// The bone is moved off the origin so that the two possible answers are two different
        /// numbers: an unskinned buffer holds the bind pose the quad was authored at, and the pose
        /// holds it moved by five.
        TEST(RtxSceneExtractorTest, aSkinnedBodyIsMirroredFromThePoseTheWalkSkinned)
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
            EXPECT_EQ(stats.mInstances, 1u);
            EXPECT_EQ(scene.getTriangleCount(), 2u);

            EXPECT_EQ(scene.getMeshPositions(0)[2], osg::Vec3f(1.0f, 1.0f, 5.0f))
                << "the bind pose moved by the bone, and not an unskinned buffer";
        }

        /// A bone that moves between frames moves the body, and it is the same body.
        ///
        /// The mesh is keyed on the drawable and not on the geometry, so the pose landing in the
        /// other half of the double buffer must not read as a second mesh.
        TEST(RtxSceneExtractorTest, aBoneThatMovesMovesTheMirroredPoseWithoutAddingAMesh)
        {
            RiggedQuad rigged;
            rigged.update(1);

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);

            extractor.extract(*rigged.mSkeleton, osg::Matrixf::identity(), 0);
            EXPECT_EQ(scene.getMeshPositions(0)[2], osg::Vec3f(1.0f, 1.0f, 0.0f));

            rigged.mBone->setMatrix(osg::Matrix::translate(0.0, 0.0, 7.0));
            rigged.update(2);

            scene.clearPlacement();
            const ExtractionStats again = extractor.extract(*rigged.mSkeleton, osg::Matrixf::identity(), 0, 1);

            EXPECT_EQ(again.mMeshesAdded, 0u);
            EXPECT_EQ(again.mMeshesReused, 1u);
            EXPECT_EQ(again.mDeformed, 1u);
            EXPECT_EQ(scene.getMeshes().size(), 1u) << "the other half of the double buffer is the same mesh";
            EXPECT_EQ(scene.getMeshPositions(0)[2], osg::Vec3f(1.0f, 1.0f, 7.0f));

            // **And the frame number cannot hold it back, which is what `Traversals` is for.** A
            // deforming drawable skins once per traversal number and hands back what it already has
            // for one it has seen, so a walk that reused a number got the pose it got the first
            // time however far the bones had moved since. The number is the extractor's own now and
            // only ever goes up, so the same frame twice — which is a doll redrawn twice while the
            // game stands on one frame — still poses twice.
            rigged.mBone->setMatrix(osg::Matrix::translate(0.0, 0.0, 99.0));
            rigged.update(3);

            scene.clearPlacement();
            extractor.extract(*rigged.mSkeleton, osg::Matrixf::identity(), 0, 1);

            EXPECT_EQ(scene.getMeshPositions(0)[2], osg::Vec3f(1.0f, 1.0f, 99.0f))
                << "a second walk on the same frame posed at a number it had already used";
        }

        /// A drawable whose geometry is not the geometry the mirror met under that address is
        /// mirrored again rather than written over the slot the first one took.
        ///
        /// **Because the map is keyed on an address and the engine reuses them.** A body part taken
        /// off and another put on lands where the first was, so the walk that meets it finds an
        /// entry describing something else. The slot is a run inside one shared vertex buffer:
        /// writing a longer mesh into it runs over the meshes that follow, which is not a wrong
        /// pose but a torn model — and in a release build the count is not asserted, so nothing
        /// says so. Changing the source geometry under one rig is the same fact without needing the
        /// allocator to hand back an address.
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

            // The quad standing next to the rig, whose vertices the overrun would land in.
            const std::vector<osg::Vec3f> before(scene.getMeshPositions(1).begin(), scene.getMeshPositions(1).end());

            // Six vertices where the slot holds four, under the same drawable.
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

                EXPECT_EQ(scene.getMeshPositions(0)[2], osg::Vec3f(1.0f, 1.0f, static_cast<float>(frame)))
                    << "the bone moved to " << frame << " and the mirrored pose did not follow";
            }
        }

        /// What `Inactive` actually does, and why the node mask beside it is what matters.
        ///
        /// **`Inactive` is not a defect, and this is what keeps that true.** It reads like a
        /// rasterizer decision the mirror inherited — `MWMechanics::Actors` sets it for actors
        /// outside the processing range — and the fear was that the mirror would go on reaching such
        /// an actor and find a skeleton refusing to move. It would: `SceneUtil::Skeleton::traverse`
        /// turns back only an **update** visitor, so the bones stop being animated while the mirror
        /// goes on skinning them, and what it places is the pose the last update left.
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
            ASSERT_EQ(scene.getMeshPositions(0)[2], osg::Vec3f(1.0f, 1.0f, 1.0f));

            // Out of range. The update traversal is turned back from here on, so the bone stops at
            // one however far the clock runs — and the mirror goes on placing the actor there.
            rigged.mSkeleton->setActive(SceneUtil::Skeleton::Inactive);

            for (unsigned int traversal = 2; traversal <= 4; ++traversal)
            {
                rigged.update(traversal);

                scene.clearPlacement();
                const ExtractionStats found = extractor.extract(*world, osg::Matrixf::identity(), 0, traversal - 1);

                ASSERT_EQ(found.mInstances, 1u) << "the mirror stopped reaching an Inactive actor on its own";
                EXPECT_EQ(scene.getMeshPositions(0)[2], osg::Vec3f(1.0f, 1.0f, 1.0f))
                    << "an Inactive skeleton animated at traversal " << traversal;
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

        /// A pose is the one thing the mesh cache does not answer: met again, it is read again — and
        /// a static drawable met again is not.
        ///
        /// The two kinds stand side by side in one graph and one pass, so the same run produces both
        /// answers and the difference cannot be a difference in how they were set up.
        ///
        /// The two culls also land in different halves of the double buffer, which is the case that
        /// keying the mesh cache on the geometry pointer would break: it would put two frozen poses
        /// of the same face in the scene and alternate between them.
        TEST(RtxSceneExtractorTest, aMorphedFaceIsReadAgainEachPassAndAStaticDrawableIsNot)
        {
            osg::ref_ptr<SceneUtil::MorphGeometry> morph = new SceneUtil::MorphGeometry;
            morph->setSourceGeometry(makeQuad());

            // The base target is what the pose starts from; the second is added at its weight. One
            // unit of z per unit of weight, so the expected value is arithmetic rather than a fit.
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

            DeformingCull cull;
            cull.setTraversalNumber(1);
            root->accept(cull);

            const ExtractionStats first = extractor.extract(*root, osg::Matrixf::identity(), 0);
            EXPECT_EQ(first.mMeshesAdded, 2u);
            EXPECT_EQ(first.mDeformed, 1u);
            EXPECT_EQ(scene.getMeshPositions(sFace)[2], osg::Vec3f(1.0f, 1.0f, 1.0f)) << "base plus one of the target";

            scene.clearPlacement();
            morph->getMorphTarget(1).setWeight(3.0f);
            morph->dirty();
            cull.setTraversalNumber(2);
            root->accept(cull);

            const ExtractionStats second = extractor.extract(*root, osg::Matrixf::identity(), 0);

            EXPECT_EQ(second.mMeshesAdded, 0u);
            EXPECT_EQ(second.mMeshesReused, 2u);
            EXPECT_EQ(second.mDeformed, 1u) << "the face, and only the face";
            EXPECT_EQ(scene.getMeshes().size(), 2u) << "the other half of the double buffer is the same mesh";

            ASSERT_EQ(scene.getDeformed().size(), 1u);
            EXPECT_EQ(scene.getDeformed()[0], sFace) << "the still quad's structure is not built again";

            EXPECT_EQ(scene.getMeshPositions(sFace)[2], osg::Vec3f(1.0f, 1.0f, 3.0f)) << "base plus three";
            EXPECT_EQ(scene.getMeshPositions(sStill)[2], osg::Vec3f(1.0f, 1.0f, 0.0f)) << "and the neighbour is intact";
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
