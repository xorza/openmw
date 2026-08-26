#include <algorithm>
#include <initializer_list>
#include <vector>

#include <gtest/gtest.h>

#include <osg/BlendFunc>
#include <osg/CullFace>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/Image>
#include <osg/Material>
#include <osg/MatrixTransform>
#include <osg/Texture2D>
#include <osg/observer_ptr>
#include <osgParticle/ConstantRateCounter>
#include <osgParticle/ModularEmitter>
#include <osgParticle/Particle>
#include <osgParticle/ParticleSystem>
#include <osgParticle/ParticleSystemUpdater>
#include <osgParticle/RadialShooter>
#include <osgUtil/UpdateVisitor>

#include <components/rtx/instancerecord.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/sceneextractor.hpp>
#include <components/rtx/shaders/scene.h>
#include <components/sceneutil/morphgeometry.hpp>
#include <components/sceneutil/riggeometry.hpp>
#include <components/sceneutil/skeleton.hpp>
#include <components/sceneutil/statesetupdater.hpp>
#include <components/surface/material.hpp>

namespace Rtx
{
    namespace
    {
        osg::ref_ptr<osg::Vec3Array> makePositions(std::initializer_list<osg::Vec3f> values)
        {
            osg::ref_ptr<osg::Vec3Array> positions = new osg::Vec3Array;
            for (const osg::Vec3f& value : values)
                positions->push_back(value);
            return positions;
        }

        osg::ref_ptr<osg::DrawElementsUInt> makeTriangles(std::initializer_list<unsigned int> indices)
        {
            osg::ref_ptr<osg::DrawElementsUInt> triangles = new osg::DrawElementsUInt(osg::PrimitiveSet::TRIANGLES);
            for (const unsigned int index : indices)
                triangles->push_back(index);
            return triangles;
        }

        /// The description on a state set, created empty where nothing has authored one yet.
        ///
        /// Every fixture here stands in for something `NifOsg` or `Terrain` built, and those author
        /// a `Surface::Material` for everything they build — so a fixture that binds a texture or
        /// sets a colour and describes neither is testing a state the content path cannot produce.
        Surface::Material& describe(osg::StateSet& state)
        {
            if (Surface::getMaterial(state) == nullptr)
                Surface::setMaterial(state, Surface::Material{});

            return *Surface::getWritableMaterial(state);
        }

        /// Binds a texture the way a loader does: the unit for the OpenGL renderer's shaders, and
        /// the role for everyone else.
        void paint(
            osg::StateSet& state, std::string_view file, Surface::TextureRole role = Surface::TextureRole::Diffuse)
        {
            osg::ref_ptr<osg::Image> image = new osg::Image;
            image->setFileName(std::string(file));

            state.setTextureAttributeAndModes(0, new osg::Texture2D(image), osg::StateAttribute::ON);
            describe(state).setTexture(role, image);
        }

        /// A unit quad in the xy plane: four vertices, two triangles.
        osg::ref_ptr<osg::Geometry> makeQuad()
        {
            osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
            geometry->setVertexArray(makePositions({
                osg::Vec3f(0.0f, 0.0f, 0.0f),
                osg::Vec3f(1.0f, 0.0f, 0.0f),
                osg::Vec3f(1.0f, 1.0f, 0.0f),
                osg::Vec3f(0.0f, 1.0f, 0.0f),
            }));
            geometry->addPrimitiveSet(makeTriangles({ 0, 1, 2, 0, 2, 3 }));
            return geometry;
        }

        TEST(RtxSceneExtractorTest, twoDrawablesBecomeTwoMeshesAndTwoInstances)
        {
            osg::ref_ptr<osg::Group> root = new osg::Group;
            root->addChild(makeQuad());
            root->addChild(makeQuad());

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);
            const ExtractionStats stats = extractor.extract(*root, osg::Matrixf::identity(), 0);

            EXPECT_EQ(stats.mMeshesAdded, 2u);
            EXPECT_EQ(stats.mMeshesReused, 0u);
            EXPECT_EQ(stats.mInstances, 2u);
            EXPECT_EQ(scene.getTriangleCount(), 4u);
        }

        /// The same geometry under two parents is one mesh and two placements. Getting this wrong is
        /// not a cosmetic waste: OpenMW's resource cache hands out the same object for every
        /// reference to a model, so a cell of a hundred identical crates would build a hundred
        /// acceleration structures.
        TEST(RtxSceneExtractorTest, sharedGeometryIsOneMeshPlacedTwice)
        {
            osg::ref_ptr<osg::Geometry> quad = makeQuad();

            osg::ref_ptr<osg::MatrixTransform> left = new osg::MatrixTransform(osg::Matrix::translate(10.0, 0.0, 0.0));
            left->addChild(quad);
            osg::ref_ptr<osg::MatrixTransform> right = new osg::MatrixTransform(osg::Matrix::translate(0.0, 20.0, 0.0));
            right->addChild(quad);

            osg::ref_ptr<osg::Group> root = new osg::Group;
            root->addChild(left);
            root->addChild(right);

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);
            const ExtractionStats stats = extractor.extract(*root, osg::Matrixf::identity(), 0);

            EXPECT_EQ(stats.mMeshesAdded, 1u);
            EXPECT_EQ(stats.mMeshesReused, 1u);
            ASSERT_EQ(scene.getInstances().size(), 2u);
            EXPECT_EQ(scene.getInstances()[0].mMesh, scene.getInstances()[1].mMesh);

            const osg::Vec3f origin(0.0f, 0.0f, 0.0f);
            EXPECT_EQ(origin * scene.getInstances()[0].mTransform, osg::Vec3f(10.0f, 0.0f, 0.0f));
            EXPECT_EQ(origin * scene.getInstances()[1].mTransform, osg::Vec3f(0.0f, 20.0f, 0.0f));
        }

        TEST(RtxSceneExtractorTest, theRootTransformIsAppliedAfterTheGraphsOwn)
        {
            osg::ref_ptr<osg::MatrixTransform> inner = new osg::MatrixTransform(osg::Matrix::scale(2.0, 2.0, 2.0));
            inner->addChild(makeQuad());

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);
            extractor.extract(*inner, osg::Matrixf::translate(0.0f, 0.0f, 5.0f), 0);

            // The quad's (1,1,0) corner doubles to (2,2,0), then rises by five.
            ASSERT_EQ(scene.getInstances().size(), 1u);
            EXPECT_EQ(osg::Vec3f(1.0f, 1.0f, 0.0f) * scene.getInstances()[0].mTransform, osg::Vec3f(2.0f, 2.0f, 5.0f));
        }

        /// The visitor accumulates the local-to-world on its way down instead of rebuilding each
        /// drawable's chain from the root, so what a chain composes to is its own property to hold.
        TEST(RtxSceneExtractorTest, nestedTransformsComposeFromTheRootDownwards)
        {
            // Outermost first: scale by two, then rotate a quarter turn about z, then move along x.
            osg::ref_ptr<osg::MatrixTransform> scale = new osg::MatrixTransform(osg::Matrix::scale(2.0, 2.0, 2.0));
            osg::ref_ptr<osg::MatrixTransform> turn
                = new osg::MatrixTransform(osg::Matrix::rotate(osg::PI_2, osg::Vec3d(0.0, 0.0, 1.0)));
            osg::ref_ptr<osg::MatrixTransform> shift = new osg::MatrixTransform(osg::Matrix::translate(3.0, 0.0, 0.0));

            scale->addChild(turn);
            turn->addChild(shift);
            shift->addChild(makeQuad());

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);
            extractor.extract(*scale, osg::Matrixf::identity(), 0);

            ASSERT_EQ(scene.getInstances().size(), 1u);
            const osg::Matrixf& place = scene.getInstances()[0].mTransform;

            // (1,0,0) shifts to (4,0,0), turns to (0,4,0), and scales to (0,8,0). Order is the whole
            // of what this asserts: composed the other way round it would be (0,2,0) moved to
            // (3,2,0), which is a different point and a plausible-looking one.
            const osg::Vec3f corner = osg::Vec3f(1.0f, 0.0f, 0.0f) * place;
            EXPECT_NEAR(corner.x(), 0.0f, 1e-4f);
            EXPECT_NEAR(corner.y(), 8.0f, 1e-4f);
            EXPECT_NEAR(corner.z(), 0.0f, 1e-4f);

            // And the origin lands where only the outer two act on the shift: (3,0,0) turned is
            // (0,3,0), scaled is (0,6,0).
            const osg::Vec3f origin = osg::Vec3f(0.0f, 0.0f, 0.0f) * place;
            EXPECT_NEAR(origin.x(), 0.0f, 1e-4f);
            EXPECT_NEAR(origin.y(), 6.0f, 1e-4f);
            EXPECT_NEAR(origin.z(), 0.0f, 1e-4f);
        }

        /// A transform that reads the visitor it is handed, the way `MWRender::CameraRelativeTransform`
        /// does to catch the eye point off a cull — and, like it, without checking for null first.
        ///
        /// **The sky is one of these, and it is why the walk hands its visitor over.**
        /// `osg::computeLocalToWorld` passes null, which is safe only because it never reaches a
        /// transform with no drawable below it; a visitor accumulating on the way down enters every
        /// one, and this crashed the game on the frame the sky first came into view.
        class VisitorReadingTransform : public osg::MatrixTransform
        {
        public:
            bool computeLocalToWorldMatrix(osg::Matrix& matrix, osg::NodeVisitor* nv) const override
            {
                mSaw = nv->getVisitorType();
                return osg::MatrixTransform::computeLocalToWorldMatrix(matrix, nv);
            }

            mutable osg::NodeVisitor::VisitorType mSaw = osg::NodeVisitor::UPDATE_VISITOR;
        };

        TEST(RtxSceneExtractorTest, aTransformThatReadsTheVisitorIsGivenOne)
        {
            osg::ref_ptr<VisitorReadingTransform> reads = new VisitorReadingTransform;
            reads->setMatrix(osg::Matrix::translate(0.0, 0.0, 4.0));
            reads->addChild(makeQuad());

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);
            extractor.extract(*reads, osg::Matrixf::identity(), 0);

            EXPECT_EQ(reads->mSaw, osg::NodeVisitor::NODE_VISITOR) << "the transform was handed a null visitor";

            // And it still placed what was under it, at the transform it asked for.
            ASSERT_EQ(scene.getPlacedCount(), 1u);
            EXPECT_EQ(osg::Vec3f(0.0f, 0.0f, 0.0f) * scene.getInstances()[0].mTransform, osg::Vec3f(0.0f, 0.0f, 4.0f));
        }

        /// An absolute reference frame replaces what is above it rather than adding to it, which is
        /// a branch inside `computeLocalToWorldMatrix` and the one thing an accumulating visitor
        /// could quietly get wrong by adding where it should overwrite.
        TEST(RtxSceneExtractorTest, anAbsoluteFrameDiscardsTheTransformsAboveIt)
        {
            osg::ref_ptr<osg::MatrixTransform> above
                = new osg::MatrixTransform(osg::Matrix::translate(100.0, 100.0, 100.0));
            osg::ref_ptr<osg::MatrixTransform> absolute
                = new osg::MatrixTransform(osg::Matrix::translate(0.0, 0.0, 7.0));
            absolute->setReferenceFrame(osg::Transform::ABSOLUTE_RF);

            above->addChild(absolute);
            absolute->addChild(makeQuad());

            // A relative sibling under the same parent, so the test also shows the frame is not
            // simply being ignored for everything.
            osg::ref_ptr<osg::MatrixTransform> relative
                = new osg::MatrixTransform(osg::Matrix::translate(0.0, 0.0, 7.0));
            relative->addChild(makeQuad());
            above->addChild(relative);

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);
            extractor.extract(*above, osg::Matrixf::identity(), 0);

            ASSERT_EQ(scene.getInstances().size(), 2u);
            const osg::Vec3f origin(0.0f, 0.0f, 0.0f);

            // The absolute one stands at its own translation and nowhere near the hundred above it.
            EXPECT_EQ(origin * scene.getInstances()[0].mTransform, osg::Vec3f(0.0f, 0.0f, 7.0f));

            // The relative one carries it.
            EXPECT_EQ(origin * scene.getInstances()[1].mTransform, osg::Vec3f(100.0f, 100.0f, 107.0f));
        }

        /// The property the incremental mirror rests on: nothing changed, so nothing is added.
        TEST(RtxSceneExtractorTest, aSecondPassOverAnUnchangedGraphAddsNothing)
        {
            osg::ref_ptr<osg::Group> root = new osg::Group;
            root->addChild(makeQuad());
            root->addChild(makeQuad());

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);
            extractor.extract(*root, osg::Matrixf::identity(), 0);

            const ExtractionStats second = extractor.extract(*root, osg::Matrixf::identity(), 0);

            EXPECT_EQ(second.mMeshesAdded, 0u);
            EXPECT_EQ(second.mMeshesReused, 2u);
            EXPECT_EQ(scene.getMeshes().size(), 2u);

            // **The property the incremental mirror rests on, in its strongest form.** The same mesh
            // at two places is still two rows of the acceleration structure — placements are not
            // deduplicated — but a second pass over an unchanged graph finds the slots those two
            // already hold rather than making two more. Nothing was added, and nothing moved.
            EXPECT_EQ(scene.getPlacedCount(), 2u);
            EXPECT_EQ(scene.getInstances().size(), 2u);

            scene.advancePlacement();
            EXPECT_EQ(extractor.extract(*root, osg::Matrixf::identity(), 0).mInstances, 2u);
            EXPECT_TRUE(scene.getMoved().empty()) << "an unchanged graph reported a placement moving";
        }

        /// **A node path does not identify a placement, and this is the case that proves it.**
        /// `SceneManager::getTemplate` hands out one node per model, so every reference to that model
        /// is walked from the same node down the same path. Without the anchor they share a slot,
        /// and a hundred crates collapse into one.
        TEST(RtxSceneExtractorTest, oneTemplateWalkedUnderTwoAnchorsIsTwoPlacements)
        {
            osg::ref_ptr<osg::Group> shared = new osg::Group;
            shared->addChild(makeQuad());

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);

            extractor.extract(*shared, osg::Matrixf::translate(10.0f, 0.0f, 0.0f), 1);
            extractor.extract(*shared, osg::Matrixf::translate(0.0f, 20.0f, 0.0f), 2);

            ASSERT_EQ(scene.getPlacedCount(), 2u);
            EXPECT_EQ(scene.getMeshes().size(), 1u) << "one model is still one mesh";

            const osg::Vec3f origin(0.0f, 0.0f, 0.0f);
            EXPECT_EQ(origin * scene.getInstances()[0].mTransform, osg::Vec3f(10.0f, 0.0f, 0.0f));
            EXPECT_EQ(origin * scene.getInstances()[1].mTransform, osg::Vec3f(0.0f, 20.0f, 0.0f));

            // And they keep their own histories. Moving one must leave the other reporting nothing —
            // sharing a slot would have the still one inherit the mover's previous transform and
            // smear across the frame.
            scene.advancePlacement();
            extractor.extract(*shared, osg::Matrixf::translate(11.0f, 0.0f, 0.0f), 1);
            extractor.extract(*shared, osg::Matrixf::translate(0.0f, 20.0f, 0.0f), 2);

            ASSERT_EQ(scene.getMoved().size(), 1u);
            EXPECT_EQ(scene.getMoved()[0], 0u);
        }

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

        /// A skeleton with one bone, and a rig bound rigidly to it.
        ///
        /// Every weight on one bone with an identity bind matrix makes the skinning arithmetic the
        /// bone's own transform and nothing else, so what a test expects is what it moved the bone
        /// by — rather than a fit against whatever a weighted sum happened to produce.
        struct RiggedQuad
        {
            osg::ref_ptr<SceneUtil::Skeleton> mSkeleton = new SceneUtil::Skeleton;
            osg::ref_ptr<osg::MatrixTransform> mBone = new osg::MatrixTransform;
            osg::ref_ptr<SceneUtil::RigGeometry> mRig = new SceneUtil::RigGeometry;
            osg::ref_ptr<osg::Geometry> mSource = makeQuad();

            RiggedQuad()
            {
                mBone->setName("bone");
                mSkeleton->addChild(mBone);

                mRig->setName("shape");
                mRig->setBoneInfo({ SceneUtil::RigGeometry::BoneInfo{
                    .mName = "bone", .mBoundSphere = {}, .mInvBindMatrix = osg::Matrixf::identity() } });
                mRig->setInfluences(std::vector<SceneUtil::RigGeometry::BoneWeights>(
                    4, SceneUtil::RigGeometry::BoneWeights{ { 0, 1.0f } }));
                mRig->setSourceGeometry(mSource);

                // **A named shape one level below the skeleton, which is the shape NIF content
                // has.** `RigGeometry::updateSkinToSkelMatrix` reads the node path backwards from
                // the trishape's own transform, and a rig hung straight off the skeleton walks off
                // the front of it.
                osg::ref_ptr<osg::Group> holder = new osg::Group;
                holder->addChild(mRig);
                mSkeleton->addChild(holder);
            }

            /// The update traversal the game runs before it mirrors anything.
            ///
            /// **`RigGeometry` finds its skeleton here and nowhere else.** It walks the node path
            /// for one, and the pose traversal the mirror uses is handed the drawable on its own —
            /// so a rig that had never been through an update would have nothing to skin against.
            void update(unsigned int traversal)
            {
                osgUtil::UpdateVisitor visitor;
                visitor.setTraversalNumber(traversal);
                mSkeleton->accept(visitor);
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

        /// A state-set controller of the shape `NifOsg` builds out of a `NiMaterialColorController`:
        /// it rewrites one attribute every time it is applied, and it hangs from whichever callback
        /// chain the content asked for.
        class ColourController : public SceneUtil::StateSetUpdater
        {
        public:
            float mRed = 0.0f;

            void setDefaults(osg::StateSet* stateset) override
            {
                stateset->setAttribute(new osg::Material, osg::StateAttribute::ON);
                Surface::setMaterial(*stateset, Surface::Material{});
            }

            void apply(osg::StateSet* stateset, osg::NodeVisitor*) override
            {
                const osg::Vec4f colour(mRed, 0.0f, 0.0f, 1.0f);
                auto* colours = static_cast<osg::Material*>(stateset->getAttribute(osg::StateAttribute::MATERIAL));
                colours->setDiffuse(osg::Material::FRONT_AND_BACK, colour);
                Surface::getWritableMaterial(*stateset)->mDiffuseColour = colour;
            }
        };

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

            // And a frame the controller said nothing new on writes nothing to the device: the
            // whole material, layer and mask table goes over when the revision moves.
            const std::uint64_t settled = scene.getShadingRevision();
            scene.clearPlacement();
            extractor.extract(*node, osg::Matrixf::identity(), 0, 2);
            EXPECT_EQ(scene.getShadingRevision(), settled) << "re-reading an unchanged state set is not a change";
        }

        /// The same controller as an update callback, which is how `NifOsg` hangs everything the
        /// content did not mark auto-play.
        ///
        /// `StateSetUpdater::applyUpdate` alternates the node's own state set between two copies of
        /// itself, one per traversal parity, so a mirror keying a material on that address adds one
        /// and sweeps one every frame for a surface that has not moved — and every placement
        /// standing on it has to be repointed each time.
        /// Morrowind scrolls lava, waterfalls and banners by moving a texture matrix rather than
        /// geometry, and the description records the two numbers that matrix was built from. The
        /// sampler takes `uv * xy + zw`, so the scale-about-the-middle has to be resolved on the way
        /// through — a surface scaled by two with no offset samples `uv * 2 - 0.5`, which is the
        /// middle of the texture staying put while its edges move outward.
        TEST(RtxSceneExtractorTest, aScrollingSurfaceCarriesItsUvTransformResolvedForTheSampler)
        {
            osg::ref_ptr<osg::Geometry> quad = makeQuad();
            paint(*quad->getOrCreateStateSet(), "lava.dds");

            Surface::Material& described = describe(*quad->getOrCreateStateSet());
            described.mTextureScale = osg::Vec2f(2.0f, 4.0f);
            described.mTextureOffset = osg::Vec2f(0.25f, -0.5f);

            osg::ref_ptr<osg::Group> root = new osg::Group;
            root->addChild(quad);

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);
            extractor.extract(*root, osg::Matrixf::identity(), 0);

            ASSERT_EQ(scene.getMaterials().size(), 1u);

            // 0.5 * (1 - 2) + 0.25 = -0.25, and 0.5 * (1 - 4) - 0.5 = -2.
            EXPECT_EQ(scene.getMaterials()[0].mTextureTransform, osg::Vec4f(2.0f, 4.0f, -0.25f, -2.0f));
        }

        /// The identity, and not by accident: every surface that does not scroll shares one sampler
        /// path with the ones that do, so the transform has to be a no-op rather than a branch.
        TEST(RtxSceneExtractorTest, aSurfaceThatDoesNotScrollCarriesTheIdentityTransform)
        {
            osg::ref_ptr<osg::Geometry> quad = makeQuad();
            paint(*quad->getOrCreateStateSet(), "stone.dds");

            osg::ref_ptr<osg::Group> root = new osg::Group;
            root->addChild(quad);

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);
            extractor.extract(*root, osg::Matrixf::identity(), 0);

            ASSERT_EQ(scene.getMaterials().size(), 1u);
            EXPECT_EQ(scene.getMaterials()[0].mTextureTransform, osg::Vec4f(1.0f, 1.0f, 0.0f, 0.0f));
        }

        TEST(RtxSceneExtractorTest, aMaterialKeepsItsSlotWhileTheNodesOwnStateSetAlternates)
        {
            osg::ref_ptr<ColourController> controller = new ColourController;

            osg::ref_ptr<osg::Group> node = new osg::Group;
            node->addChild(makeQuad());
            node->addUpdateCallback(controller);

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);

            osgUtil::UpdateVisitor update;

            for (unsigned int pass = 1; pass <= 4; ++pass)
            {
                update.setTraversalNumber(pass);
                node->accept(update);

                scene.clearPlacement();
                const ExtractionStats found = extractor.extract(*node, osg::Matrixf::identity(), 0, pass);
                const Retirement went = extractor.retire();

                // The first pass is where everything arrives; what is asserted is that no later one
                // is, and the parity has turned over twice by the last.
                if (pass == 1)
                    continue;

                EXPECT_EQ(found.mMaterialsAdded, 0u) << "pass " << pass;
                EXPECT_EQ(found.mMaterialsReused, 1u) << "pass " << pass;
                EXPECT_EQ(went.mMaterials, 0u) << "pass " << pass << ": swept is added again next frame";
            }

            EXPECT_EQ(scene.getMaterials().size(), 1u);
        }

        TEST(RtxSceneExtractorTest, degenerateTrianglesAreDropped)
        {
            // One real triangle and two zero-area ones, which is how a triangle strip restarts.
            osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
            geometry->setVertexArray(makePositions({
                osg::Vec3f(0.0f, 0.0f, 0.0f),
                osg::Vec3f(1.0f, 0.0f, 0.0f),
                osg::Vec3f(1.0f, 1.0f, 0.0f),
            }));
            geometry->addPrimitiveSet(makeTriangles({ 0, 1, 2, 0, 0, 1, 2, 2, 2 }));

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);
            extractor.extract(*geometry, osg::Matrixf::identity(), 0);

            EXPECT_EQ(scene.getTriangleCount(), 1u);
        }

        TEST(RtxSceneExtractorTest, geometryWithNoTrianglesIsSkippedRatherThanAdded)
        {
            osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
            geometry->setVertexArray(makePositions({ osg::Vec3f(0.0f, 0.0f, 0.0f) }));

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);
            const ExtractionStats stats = extractor.extract(*geometry, osg::Matrixf::identity(), 0);

            EXPECT_EQ(stats.mSkippedEmpty, 1u);
            EXPECT_EQ(stats.mInstances, 0u);
            EXPECT_TRUE(scene.getMeshes().empty());
        }

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
        TEST(RtxSceneExtractorTest, aDrawableTheCallerCallsWaterIsShadedAsWaterAndTheRestAreNot)
        {
            constexpr osg::Node::NodeMask sWater = 1u << 6;
            constexpr osg::Node::NodeMask sOther = 1u << 3;

            const auto quadWith = [&](osg::Node::NodeMask mask) {
                osg::ref_ptr<osg::Geometry> quad = makeQuad();
                quad->setNodeMask(mask);

                // A state set of its own, because a material is keyed on one and two quads sharing
                // one would be one material between them.
                paint(*quad->getOrCreateStateSet(), "textures/water/water00.dds");

                return quad;
            };

            osg::ref_ptr<osg::Group> root = new osg::Group;
            root->addChild(quadWith(sWater));
            root->addChild(quadWith(sOther));

            // **A drawable that never set a mask, which is nearly every one in the game.** OSG
            // defaults a node mask to all ones, so a test that asks whether the water's bit is
            // *among* a drawable's bits says yes to all of them — and the whole world came out
            // shaded as sea, refracting like jelly. What names the water is that no other pass may
            // see it.
            root->addChild(quadWith(~osg::Node::NodeMask{ 0 }));

            // Nothing said, so nothing is water — which is the harness, and every caller that places
            // an analytic sea of its own instead.
            {
                Rtx::SceneDesc scene;
                SceneExtractor silent(scene);
                silent.extract(*root, osg::Matrixf::identity(), 0);

                ASSERT_EQ(scene.getMaterials().size(), 3u);
                for (const Rtx::Material& material : scene.getMaterials())
                    EXPECT_EQ(material.mKind, Rtx::MaterialKind::Surface);
            }

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);
            extractor.setWaterMask(sWater);
            extractor.extract(*root, osg::Matrixf::identity(), 0);

            ASSERT_EQ(scene.getMaterials().size(), 3u);
            EXPECT_EQ(scene.getMaterials()[0].mKind, Rtx::MaterialKind::Water);
            EXPECT_EQ(scene.getMaterials()[1].mKind, Rtx::MaterialKind::Surface)
                << "a mask the caller did not name made a surface into a sea";
            EXPECT_EQ(scene.getMaterials()[2].mKind, Rtx::MaterialKind::Surface)
                << "a drawable with the default mask was called water, which is every drawable";

            // **What being water is actually for.** A shadow ray has to pass through the surface, or
            // every shallow in the game is lit as though the sea were a wall; the mask is where the
            // record says so, and the material kind is where it comes from.
            std::vector<Rtx::InstanceRecord> records;
            Rtx::makeInstanceRecords(scene, records);

            ASSERT_EQ(records.size(), 3u);
            EXPECT_EQ(records[0].mMask, Rtx::Shaders::MASK_WATER);
            EXPECT_EQ(records[1].mMask, Rtx::Shaders::MASK_SOLID);
            EXPECT_EQ(records[2].mMask, Rtx::Shaders::MASK_SOLID);
            EXPECT_NE(records[0].mMask, records[1].mMask);
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

        /// A particle system under a transform that carries its texture and its blend, the way
        /// `NifOsg` builds one.
        ///
        /// The emitter's own state set sets neither, which is what makes this a test of the walk up
        /// the path rather than of the drawable: a `ParticleSystem` really does carry an empty state
        /// set of its own in the shipped content, and asking it for the blend answers "covers" for
        /// every flame in the game.
        struct Plume
        {
            osg::ref_ptr<osg::MatrixTransform> mRoot;
            osg::ref_ptr<osgParticle::ParticleSystem> mParticles;
        };

        Plume makePlume(const osg::Matrix& place, bool additive)
        {
            Plume plume;
            plume.mRoot = new osg::MatrixTransform(place);

            osg::StateSet& state = *plume.mRoot->getOrCreateStateSet();
            paint(state, "textures/tx_fire_00.dds");
            state.setAttributeAndModes(new osg::BlendFunc(osg::BlendFunc::SRC_ALPHA,
                                           additive ? osg::BlendFunc::ONE : osg::BlendFunc::ONE_MINUS_SRC_ALPHA),
                osg::StateAttribute::ON);
            describe(state).mAlphaMode = Surface::AlphaMode::Blend;

            plume.mParticles = new osgParticle::ParticleSystem;
            plume.mParticles->getOrCreateStateSet()->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
            plume.mRoot->addChild(plume.mParticles);

            return plume;
        }

        /// Adds one particle and brings its interpolated size, colour and alpha up to date.
        ///
        /// `getCurrentSize` and the two beside it are only meaningful after `Particle::update`, so
        /// the zero-length step is not a formality: without it every sprite this test reads back
        /// carries whatever the default template was constructed with.
        osgParticle::Particle* emit(
            osgParticle::ParticleSystem& particles, const osg::Vec3f& at, float size, const osg::Vec4f& colour)
        {
            osgParticle::Particle seed;
            osgParticle::Particle* particle = particles.createParticle(&seed);
            particle->setLifeTime(10.0f);
            particle->setPosition(at);
            particle->setVelocity(osg::Vec3f());
            particle->setSizeRange(osgParticle::rangef(size, size));
            particle->setColorRange(osgParticle::rangev4(colour, colour));
            particle->setAlphaRange(osgParticle::rangef(colour.a(), colour.a()));
            particle->update(0.0, false);
            return particle;
        }

        /// A particle system is not geometry, and what comes out of it is a run of discs.
        ///
        /// Every number here is the file's own carried through one transform: the placement moves
        /// each sprite and its uniform scale widens it, because `NifOsg` asks for particle sizes in
        /// the emitter's own coordinates and the modelview is what the rasterizer would have scaled
        /// them by.
        TEST(RtxSceneExtractorTest, aParticleSystemPlacesSpritesAndNoMesh)
        {
            // Scaled by two and moved a hundred along x, so the radius and the position each prove a
            // different half of the transform.
            const Plume plume = makePlume(osg::Matrix::scale(2.0, 2.0, 2.0) * osg::Matrix::translate(100.0, 0.0, 0.0),
                /*additive=*/true);

            emit(*plume.mParticles, osg::Vec3f(0.0f, 0.0f, 5.0f), 3.0f, osg::Vec4f(1.0f, 0.5f, 0.25f, 0.5f));
            emit(*plume.mParticles, osg::Vec3f(0.0f, 0.0f, 9.0f), 1.0f, osg::Vec4f(1.0f, 1.0f, 1.0f, 1.0f));

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);
            const ExtractionStats stats = extractor.extract(*plume.mRoot, osg::Matrixf::identity(), 0);

            EXPECT_EQ(stats.mEmitters, 1u);
            EXPECT_EQ(stats.mSprites, 2u);
            EXPECT_EQ(stats.mSkippedUnknown, 0u) << "a particle system is read, not passed over";
            EXPECT_EQ(stats.mInstances, 0u) << "sprites are the drawing, so there is nothing to build over";
            EXPECT_EQ(stats.mMeshesAdded, 0u);

            ASSERT_EQ(scene.getSprites().size(), 2u);

            // (0, 0, 5) scaled by two is (0, 0, 10), then moved to x = 100. The radius is the file's
            // three by the same two.
            const Rtx::Sprite& low = scene.getSprites()[0];
            EXPECT_EQ(low.mPosition, osg::Vec3f(100.0f, 0.0f, 10.0f));
            EXPECT_FLOAT_EQ(low.mRadius, 6.0f);
            EXPECT_EQ(low.mColour, osg::Vec3f(1.0f, 0.5f, 0.25f));

            // The colour ramp's alpha and the alpha ramp are separate and the rasterizer multiplies
            // them; here both are a half, so a quarter is what proves the product rather than one of
            // the two being read and the other dropped.
            EXPECT_FLOAT_EQ(low.mAlpha, 0.25f);

            EXPECT_EQ(scene.getSprites()[1].mPosition, osg::Vec3f(100.0f, 0.0f, 18.0f));
            EXPECT_FLOAT_EQ(scene.getSprites()[1].mRadius, 2.0f);

            // Two sprites four apart before the scale and eight after, each one wider than the
            // other: the box runs z = 4 to 20, so the centre is 12 and the reach 8.
            ASSERT_EQ(scene.getEmitters().size(), 1u);
            EXPECT_EQ(scene.getEmitters().front().mCentre, osg::Vec3f(100.0f, 0.0f, 12.0f));
            EXPECT_FLOAT_EQ(scene.getEmitters().front().mReach, 8.0f);
            EXPECT_EQ(scene.getTextures().size(), 1u);
            EXPECT_EQ(scene.getTextures()[0], VFS::Path::NormalizedView("textures/tx_fire_00.dds"));
        }

        /// `SRC_ALPHA, ONE` is a flame and anything else covers, and the difference is what decides
        /// whether the sprite is light or an albedo to be lit.
        ///
        /// The blend sits on the transform above the emitter, where `NifOsg` puts it, and the
        /// emitter carries a state set of its own that says nothing about blending — so an answer
        /// read off the drawable is "covers" both times.
        TEST(RtxSceneExtractorTest, theBlendTellsAFlameFromSmoke)
        {
            const auto extractOne = [](bool additive) {
                const Plume plume = makePlume(osg::Matrix::identity(), additive);
                emit(*plume.mParticles, osg::Vec3f(), 1.0f, osg::Vec4f(1.0f, 1.0f, 1.0f, 1.0f));

                Rtx::SceneDesc scene;
                SceneExtractor extractor(scene);
                extractor.extract(*plume.mRoot, osg::Matrixf::identity(), 0);

                EXPECT_EQ(scene.getEmitters().size(), 1u);
                return scene.getEmitters().front().mAdditive;
            };

            EXPECT_TRUE(extractOne(true));
            EXPECT_FALSE(extractOne(false));
        }

        /// A dead slot keeps the position its last particle expired at, and an emitter with nothing
        /// alive places nothing at all — not a sphere with an empty run behind it, which every ray
        /// crossing that part of the cell would then be rejected by one test later than it needs.
        TEST(RtxSceneExtractorTest, deadParticlesAndUntexturedEmittersPlaceNothing)
        {
            const Plume spent = makePlume(osg::Matrix::identity(), true);
            osgParticle::Particle* particle
                = emit(*spent.mParticles, osg::Vec3f(0.0f, 0.0f, 5.0f), 3.0f, osg::Vec4f(1.0f, 1.0f, 1.0f, 1.0f));
            particle->kill();
            particle->update(0.0, false);
            ASSERT_FALSE(particle->isAlive());

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);
            const ExtractionStats stats = extractor.extract(*spent.mRoot, osg::Matrixf::identity(), 0);

            EXPECT_EQ(stats.mEmitters, 0u);
            EXPECT_EQ(stats.mSprites, 0u);
            EXPECT_TRUE(scene.getEmitters().empty());

            // The texture is registered the moment the emitter is met, alive or not: it is what the
            // array is built from, and one that turns up two hundred frames later has nowhere to go.
            EXPECT_EQ(scene.getTextures().size(), 1u);

            // A particle's whole silhouette is that texture's alpha, so an emitter with none draws
            // nothing rather than a white disc.
            osg::ref_ptr<osg::Group> bare = new osg::Group;
            osg::ref_ptr<osgParticle::ParticleSystem> particles = new osgParticle::ParticleSystem;
            bare->addChild(particles);
            emit(*particles, osg::Vec3f(), 1.0f, osg::Vec4f(1.0f, 1.0f, 1.0f, 1.0f));

            Rtx::SceneDesc bareScene;
            SceneExtractor bareExtractor(bareScene);
            EXPECT_EQ(bareExtractor.extract(*bare, osg::Matrixf::identity(), 0).mEmitters, 0u);
            EXPECT_TRUE(bareScene.getTextures().empty());
        }

        /// An emitter's sprite is on no material, so the sweep has to speak for it itself.
        ///
        /// The emitter outlives a textured quad here, and the texture the quad wore is what proves
        /// the sweep is doing anything at all: a pass that kept every texture would keep both.
        TEST(RtxSceneExtractorTest, aSweepKeepsTheTextureAnEmitterIsStillDrawingWith)
        {
            osg::ref_ptr<osg::Geometry> stone = makeQuad();
            paint(*stone->getOrCreateStateSet(), "textures/tx_stone_01.dds");

            const Plume plume = makePlume(osg::Matrix::identity(), /*additive=*/true);
            emit(*plume.mParticles, osg::Vec3f(), 1.0f, osg::Vec4f(1.0f, 1.0f, 1.0f, 1.0f));

            osg::ref_ptr<osg::Group> both = new osg::Group;
            both->addChild(stone);
            both->addChild(plume.mRoot);

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);
            extractor.extract(*both, osg::Matrixf::identity(), 0);
            ASSERT_EQ(scene.getTextures().size(), 2u);
            ASSERT_TRUE(extractor.retire().empty());

            scene.clearPlacement();
            extractor.extract(*plume.mRoot, osg::Matrixf::identity(), 0);

            const Retirement went = extractor.retire();

            EXPECT_EQ(went.mMeshes, 1u) << "the stone the second walk did not meet";
            EXPECT_EQ(went.mMaterials, 1u);

            // No slot is reclaimed from the table — nothing is renumbered — so what this asserts is
            // that the sprite's texture is still *named*, which is the thing the emitter map exists
            // for: a sprite hangs off no material, so nothing else holds it. The stone's went with
            // the stone's material, which is the other half of the same statement.
            ASSERT_EQ(scene.getTextures().size(), 2u);
            EXPECT_TRUE(scene.getTextures()[0].value().empty()) << "the stone's texture outlived the stone";
            EXPECT_EQ(scene.getTextures()[1], VFS::Path::NormalizedView("textures/tx_fire_00.dds"));

            // And the emitter still draws with it.
            scene.clearPlacement();
            extractor.extract(*plume.mRoot, osg::Matrixf::identity(), 0);

            ASSERT_EQ(scene.getEmitters().size(), 1u);
            EXPECT_EQ(scene.getEmitters().front().mTexture, 1u) << "the sprite lost the slot it was given";
            EXPECT_EQ(scene.getTextures().size(), 2u) << "the sprite's path was added a second time";

            // **And the other way round, on the frame the sweep does not look at.** The stone comes
            // back and then the emitter goes, taking no mesh and no material with it — which is
            // exactly the frame `SceneDesc::release` answers with two comparisons and returns from.
            // The sprite's slot has to be given back by whatever was holding it.
            scene.clearPlacement();
            extractor.extract(*both, osg::Matrixf::identity(), 0);
            ASSERT_TRUE(extractor.retire().empty());
            ASSERT_EQ(scene.getTextures()[0], VFS::Path::NormalizedView("textures/tx_stone_01.dds"));

            scene.clearPlacement();
            extractor.extract(*stone, osg::Matrixf::identity(), 0);

            EXPECT_TRUE(extractor.retire().empty()) << "an emitter is neither a mesh nor a material";
            EXPECT_TRUE(scene.getTextures()[1].value().empty()) << "the sprite outlived the emitter";
        }

        /// Gives a plume what makes it run: something emitting at a fixed rate, and the updater
        /// that integrates what it emitted.
        ///
        /// Both go in above the system they drive, which is where `NifOsg` puts them and where
        /// `osgParticle` needs them — a walk that meets the updater first reads a system already
        /// integrated this frame rather than one frame of staleness.
        ///
        /// Frozen on cull to start with, because that is how a system arrives from a file and how a
        /// system that has never been drawn stays: `_last_frame` moves in `drawImplementation` and
        /// nowhere else, so nothing here would ever advance it.
        void drive(Plume& plume, double perSecond, bool updaterAbove = true)
        {
            plume.mParticles->setFreezeOnCull(true);

            osgParticle::Particle& seed = plume.mParticles->getDefaultParticleTemplate();
            seed.setLifeTime(10.0f);
            seed.setSizeRange(osgParticle::rangef(2.0f, 2.0f));
            seed.setAlphaRange(osgParticle::rangef(1.0f, 1.0f));
            seed.setColorRange(
                osgParticle::rangev4(osg::Vec4f(1.0f, 1.0f, 1.0f, 1.0f), osg::Vec4f(1.0f, 1.0f, 1.0f, 1.0f)));

            osg::ref_ptr<osgParticle::ConstantRateCounter> counter = new osgParticle::ConstantRateCounter;
            counter->setNumberOfParticlesPerSecondToCreate(perSecond);

            // Straight up at a fixed speed, so that where a particle has got to is a number this
            // test can compare rather than a random direction.
            osg::ref_ptr<osgParticle::RadialShooter> shooter = new osgParticle::RadialShooter;
            shooter->setThetaRange(0.0f, 0.0f);
            shooter->setPhiRange(0.0f, 0.0f);
            shooter->setInitialSpeedRange(100.0f, 100.0f);
            shooter->setInitialRotationalSpeedRange(osg::Vec3f(), osg::Vec3f());

            osg::ref_ptr<osgParticle::ModularEmitter> emitter = new osgParticle::ModularEmitter;
            emitter->setParticleSystem(plume.mParticles);
            emitter->setCounter(counter);
            emitter->setShooter(shooter);

            osg::ref_ptr<osgParticle::ParticleSystemUpdater> updater = new osgParticle::ParticleSystemUpdater;
            updater->addParticleSystem(plume.mParticles);

            plume.mRoot->insertChild(0, emitter);
            if (updaterAbove)
                plume.mRoot->insertChild(1, updater);
            else
                plume.mRoot->addChild(updater);
        }

        /// Every sprite the scene holds, by height.
        std::vector<float> spriteHeights(const Rtx::SceneDesc& scene)
        {
            std::vector<float> heights;
            for (const Rtx::Sprite& sprite : scene.getSprites())
                heights.push_back(sprite.mPosition.z());

            std::sort(heights.begin(), heights.end());
            return heights;
        }

        /// Where an emitter's particles have got to does not depend on where its updater sits.
        ///
        /// **`osgParticle` splits emission from integration across two sibling nodes**, so a walk
        /// that reads the particles as it passes them reads a different frame's worth depending on
        /// which sibling comes first. `NifOsg` puts the updater above the system deliberately and
        /// every model in the game obeys that, which makes the dependency invisible right up until
        /// something hand-built does not — and then it is one frame of staleness in a position,
        /// which nothing will ever notice. Reading after the walk has settled removes the question,
        /// and this is the assertion that says so.
        TEST(RtxSceneExtractorTest, spritesAreReadAfterTheWalkRatherThanAsItPassesThem)
        {
            const auto run = [](bool updaterAbove) {
                Plume plume = makePlume(osg::Matrix::identity(), /*additive=*/true);
                drive(plume, 100.0, updaterAbove);

                Rtx::SceneDesc scene;
                SceneExtractor extractor(scene);

                // The first turn only starts the clock; the second emits and integrates.
                for (int turn = 0; turn < 2; ++turn)
                {
                    scene.clearPlacement();
                    extractor.advanceEmitters(0.1);
                    extractor.extract(*plume.mRoot, osg::Matrixf::identity(), 0);
                }

                return spriteHeights(scene);
            };

            const std::vector<float> above = run(true);
            const std::vector<float> below = run(false);

            ASSERT_EQ(above.size(), 10u);
            EXPECT_EQ(above, below) << "the graph's order decided what a sprite's position was";

            // **And they have actually moved**, so that the agreement above is two settled reads
            // rather than two stale ones. A particle shot straight up at a hundred units a second
            // is at least a whole tenth-second step off the placer's origin, which is ten.
            EXPECT_GE(above.front(), 10.0f);
        }

        /// An emitter runs, and it runs once per turn of the emitter clock however often it is walked.
        ///
        /// **`osgParticle` hangs its whole simulation off the cull traversal**, and a ray tracer has
        /// none — so without the walk claiming that name at the two nodes that ask, every particle
        /// system in the world stands still on the seed its file was authored with. That failure is
        /// silent: the scene still has an emitter in it and still places sprites, they just never
        /// change, which is why this asserts on a count that moves rather than on one that exists.
        TEST(RtxSceneExtractorTest, anEmitterRunsOnTheEmitterClockAndOnlyOncePerTurnOfIt)
        {
            Plume plume = makePlume(osg::Matrix::identity(), /*additive=*/true);
            drive(plume, 100.0);

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);

            // **The first turn only starts the clock.** `ParticleProcessor` keeps the last time it
            // saw and has none yet, so it records one and steps nothing — which is also why a
            // renderer that walks a cell once and shows it has to warm its emitters first.
            extractor.advanceEmitters(0.1);
            EXPECT_EQ(extractor.extract(*plume.mRoot, osg::Matrixf::identity(), 0).mSprites, 0u);

            EXPECT_FALSE(plume.mParticles->getFreezeOnCull())
                << "freeze-on-cull asks whether the draw has touched this, and nothing here draws";

            // A tenth of a second at a hundred a second is ten.
            extractor.advanceEmitters(0.1);
            EXPECT_EQ(extractor.extract(*plume.mRoot, osg::Matrixf::identity(), 0).mSprites, 10u);

            // **The same ten, and not another ten.** Every walk that reaches an emitter says it is a
            // cull traversal, and the game reaches this one twice a frame — the world's walk and the
            // weather's. What keeps that one step is `ParticleProcessor`'s own once-per-frame guard,
            // and it only holds while a single clock is writing it.
            EXPECT_EQ(extractor.extract(*plume.mRoot, osg::Matrixf::identity(), 0).mSprites, 10u);

            // Ten more on the next turn, so the guard is a guard and not a stop.
            extractor.advanceEmitters(0.1);
            EXPECT_EQ(extractor.extract(*plume.mRoot, osg::Matrixf::identity(), 0).mSprites, 20u);
        }

        /// A gap in the world's clock is clamped rather than emitted.
        ///
        /// A loading screen, a paused window or a harness holding the world still are each a gap an
        /// emitter would take literally, and a literal hour at a hundred a second is three hundred
        /// and sixty thousand particles in one frame.
        TEST(RtxSceneExtractorTest, aJumpInTheWorldsClockIsClampedRatherThanEmitted)
        {
            Plume plume = makePlume(osg::Matrix::identity(), /*additive=*/true);
            drive(plume, 100.0);

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);

            extractor.advanceEmitters(0.1);
            extractor.extract(*plume.mRoot, osg::Matrixf::identity(), 0);

            // Clamped to the two tenths the game's own frame loop caps a step at: twenty, not
            // 360,000.
            extractor.advanceEmitters(3600.0);
            EXPECT_EQ(extractor.extract(*plume.mRoot, osg::Matrixf::identity(), 0).mSprites, 20u);

            // And a step backwards is not a step backwards, it is no step at all.
            extractor.advanceEmitters(-10.0);
            EXPECT_EQ(extractor.extract(*plume.mRoot, osg::Matrixf::identity(), 0).mSprites, 20u);
        }

        /// The emitters can be run without a frame being mirrored, which is what a warm-up is.
        TEST(RtxSceneExtractorTest, emittersCanBeSteppedWithoutMirroringAnything)
        {
            Plume plume = makePlume(osg::Matrix::identity(), /*additive=*/true);
            drive(plume, 100.0);

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);

            for (int turn = 0; turn < 3; ++turn)
            {
                extractor.advanceEmitters(0.1);
                extractor.stepEmitters(*plume.mRoot);
            }

            EXPECT_EQ(scene.getSprites().size(), 0u) << "stepping is not mirroring: nothing was placed";

            // Two of those three turns emitted — the first only started the clock — and the walk that
            // finally mirrors them adds none of its own, because its turn is already spent.
            EXPECT_EQ(extractor.extract(*plume.mRoot, osg::Matrixf::identity(), 0).mSprites, 20u);
        }

        /// A drawable that describes nothing inherits the nearest description above it.
        ///
        /// **Nearest, and whole.** A NIF property on a node applies to every shape below it until
        /// another replaces it, so `NifOsg` resolves each shape against everything above and stamps
        /// one complete answer. Walking back up for the first description found reproduces that,
        /// and a drawable carrying a state set for some unrelated reason — a `CullFace` and nothing
        /// else, which is common — does not lose the surface it inherits by having one.
        TEST(RtxSceneExtractorTest, aDrawableWithNoDescriptionInheritsTheNearestOneAbove)
        {
            osg::ref_ptr<osg::Group> parent = new osg::Group;
            paint(*parent->getOrCreateStateSet(), "textures/tx_stone_01.dds");

            osg::ref_ptr<osg::Geometry> quad = makeQuad();
            quad->getOrCreateStateSet()->setAttributeAndModes(
                new osg::CullFace(osg::CullFace::BACK), osg::StateAttribute::ON);
            parent->addChild(quad);

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);
            extractor.extract(*parent, osg::Matrixf::identity(), 0);

            ASSERT_EQ(scene.getMaterials().size(), 1u);
            ASSERT_EQ(scene.getTextures().size(), 1u);
            EXPECT_EQ(scene.getTextures()[0], VFS::Path::NormalizedView("textures/tx_stone_01.dds"));
            EXPECT_EQ(scene.getMaterials()[0].mDiffuse, 0u);

            // **The description's answer and not the drawable's pipeline state.** The quad carries
            // a `CullFace(BACK)` and the description above it says nothing about faces, so it is
            // two-sided — which is what the description was introduced to make true. Reading the
            // attribute back off the state set, as the mirror used to, would answer the other way.
            EXPECT_TRUE(scene.getMaterials()[0].mTwoSided);
        }

        /// A blend is what marks a cutout in this data, and it has to survive into the material.
        ///
        /// Morrowind's foliage, grates and banners are drawn with `NiAlphaProperty` over a texture
        /// whose alpha is all but binary; hardly anything in the game sets an alpha test. Losing
        /// the blend here loses every mask with it.
        TEST(RtxSceneExtractorTest, aBlendedSurfaceIsTracedAsACutoutAndAPlainOneIsNot)
        {
            const auto extractOne = [](bool blend) {
                osg::ref_ptr<osg::Geometry> quad = makeQuad();
                osg::StateSet& state = *quad->getOrCreateStateSet();
                paint(state, "textures/tx_leaves.dds");
                if (blend)
                {
                    state.setAttributeAndModes(new osg::BlendFunc, osg::StateAttribute::ON);
                    describe(state).mAlphaMode = Surface::AlphaMode::Blend;
                }

                Rtx::SceneDesc scene;
                SceneExtractor extractor(scene);
                extractor.extract(*quad, osg::Matrixf::identity(), 0);

                EXPECT_EQ(scene.getMaterials().size(), 1u);
                return scene.getMaterials().front();
            };

            const Rtx::Material blended = extractOne(true);
            EXPECT_EQ(blended.mAlphaMode, Rtx::AlphaMode::Blend);
            EXPECT_TRUE(blended.isCutout());

            const Rtx::Material plain = extractOne(false);
            EXPECT_EQ(plain.mAlphaMode, Rtx::AlphaMode::Opaque);
            EXPECT_FALSE(plain.isCutout());
        }

        /// The emissive multiplier is folded into the colour, because their product is all the
        /// game's own shader ever uses.
        TEST(RtxSceneExtractorTest, anEmissiveMultiplierIsFoldedIntoTheColourItScales)
        {
            const auto extractOne = [](float multiplier) {
                osg::ref_ptr<osg::Geometry> quad = makeQuad();
                osg::StateSet& state = *quad->getOrCreateStateSet();

                Surface::Material& surface = describe(state);
                surface.mEmissiveColour = osg::Vec3f(0.5f, 0.25f, 0.0f);
                surface.mEmissiveMult = multiplier;

                Rtx::SceneDesc scene;
                SceneExtractor extractor(scene);
                extractor.extract(*quad, osg::Matrixf::identity(), 0);

                EXPECT_EQ(scene.getMaterials().size(), 1u);
                return scene.getMaterials().front().mEmissiveColour;
            };

            EXPECT_EQ(extractOne(2.0f), osg::Vec3f(1.0f, 0.5f, 0.0f));
            EXPECT_EQ(extractOne(0.5f), osg::Vec3f(0.25f, 0.125f, 0.0f));

            // The default is one, so a model that asked for nothing keeps the colour it authored.
            EXPECT_EQ(extractOne(1.0f), osg::Vec3f(0.5f, 0.25f, 0.0f));
        }

        /// Two-sidedness is what the content said, not what the pipeline state happens to be.
        ///
        /// **This is the fact that used to be guessed.** OpenGL culls nothing unless told to and
        /// `NifOsg` only emitted a `CullFace` where a `NiStencilProperty` asked for one, so an
        /// absent attribute had to be read as two-sided — which is right for a sheet of vanilla
        /// foliage and wrong for everything under a scene root that turns culling on globally. The
        /// description says which, and says it whether or not any state set mentions culling.
        TEST(RtxSceneExtractorTest, aSurfaceIsTwoSidedWhenTheContentSaidSo)
        {
            const auto extractOne = [](bool twoSided) {
                osg::ref_ptr<osg::Geometry> quad = makeQuad();
                describe(*quad->getOrCreateStateSet()).mTwoSided = twoSided;

                Rtx::SceneDesc scene;
                SceneExtractor extractor(scene);
                extractor.extract(*quad, osg::Matrixf::identity(), 0);

                EXPECT_EQ(scene.getMaterials().size(), 1u);
                return scene.getMaterials()[0].mTwoSided;
            };

            EXPECT_TRUE(extractOne(true));
            EXPECT_FALSE(extractOne(false));
        }

        /// A surface nothing described is a canary rather than a guess.
        ///
        /// Every state set the content pipeline produces carries a description; one that does not
        /// was built somewhere else, or was rebuilt by something that copied the pipeline state and
        /// dropped the description with it. The extractor says so and does not try to recover it.
        TEST(RtxSceneExtractorTest, anUndescribedSurfaceIsCountedRatherThanGuessedAt)
        {
            osg::ref_ptr<osg::Geometry> quad = makeQuad();
            quad->getOrCreateStateSet()->setAttributeAndModes(
                new osg::CullFace(osg::CullFace::BACK), osg::StateAttribute::ON);

            Rtx::SceneDesc scene;
            SceneExtractor extractor(scene);
            const ExtractionStats stats = extractor.extract(*quad, osg::Matrixf::identity(), 0);

            EXPECT_EQ(stats.mUndescribedMaterials, 1u);
            EXPECT_EQ(stats.mInstances, 1u) << "the geometry is still placed; only its shading is unknown";
            ASSERT_EQ(scene.getMaterials().size(), 1u);
            EXPECT_EQ(scene.getMaterials()[0].mDiffuse, Rtx::sNoIndex);
        }
    }
}
