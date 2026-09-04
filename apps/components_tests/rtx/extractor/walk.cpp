#include "fixture.hpp"

#include <vector>

namespace Rtx::Testing
{
    namespace
    {
        /// Says what the loader answers about a hidden node, and puts back what the binary had.
        ///
        /// **Process-global, and `SceneExtractor` reads it when it is built**, so a test that left
        /// its own answer behind would move the default of every extractor built after it.
        class ConfiguredLoader
        {
        public:
            explicit ConfiguredLoader(unsigned int hiddenNodeMask)
                : mHeld{ .mHiddenNodeMask = NifOsg::Loader::getHiddenNodeMask(),
                    .mIntersectionDisabledNodeMask = NifOsg::Loader::getIntersectionDisabledNodeMask(),
                    .mSoftEffects = NifOsg::Loader::getSoftEffectEnabled(),
                    .mShowMarkers = NifOsg::Loader::getShowMarkers() }
            {
                NifOsg::Loader::Configuration asked = mHeld;
                asked.mHiddenNodeMask = hiddenNodeMask;
                NifOsg::Loader::configure(asked);
            }

            ~ConfiguredLoader() { NifOsg::Loader::configure(mHeld); }

            ConfiguredLoader(const ConfiguredLoader&) = delete;
            ConfiguredLoader& operator=(const ConfiguredLoader&) = delete;

        private:
            NifOsg::Loader::Configuration mHeld;
        };

        TEST_F(RtxSceneExtractorTest, twoDrawablesBecomeTwoMeshesAndTwoInstances)
        {
            osg::ref_ptr<osg::Group> root = new osg::Group;
            root->addChild(makeQuad());
            root->addChild(makeQuad());

            const ExtractionStats stats = walk(*root);

            EXPECT_EQ(stats.mMeshesAdded, 2u);
            EXPECT_EQ(stats.mMeshesReused, 0u);
            EXPECT_EQ(stats.mInstances, 2u);
            EXPECT_EQ(mScene.getTriangleCount(), 4u);
        }

        /// A flipbook shows one frame at a time, and this walk is what advances it.
        ///
        /// **`NifOsg` builds an `osg::Sequence` for every `NiFltAnimationNode`** — Morrowind's fires,
        /// forges and lava flows. A sequence hands over every child under `TRAVERSE_ALL_CHILDREN`,
        /// so all of its frames are traced at once and in the same place, and the branch of
        /// `Sequence::traverse` that moves its clock is never reached.
        ///
        /// Two frames a second apart, read at two times. One instance each time, and the second
        /// read is the other frame. Both halves are claimed: honouring it without stepping it shows
        /// frame zero for ever, and stepping it without honouring it shows both frames at once.
        TEST_F(RtxSceneExtractorTest, aFlipbookShowsOneFrameAndThisWalkIsWhatAdvancesIt)
        {
            osg::ref_ptr<osg::MatrixTransform> first = new osg::MatrixTransform(osg::Matrix::translate(10.0, 0.0, 0.0));
            first->addChild(makeQuad());
            osg::ref_ptr<osg::MatrixTransform> second
                = new osg::MatrixTransform(osg::Matrix::translate(0.0, 20.0, 0.0));
            second->addChild(makeQuad());

            // What `NifOsg::LoaderImpl` builds for a looping two-frame node lasting two seconds.
            osg::ref_ptr<osg::Sequence> frames = new osg::Sequence;
            frames->addChild(first);
            frames->addChild(second);
            frames->setDefaultTime(1.0);
            frames->setInterval(osg::Sequence::LOOP, 0, -1);
            frames->setDuration(1.0f, -1);
            frames->setMode(osg::Sequence::START);

            // A scene of its own each time, because the claim is what one walk put there. The clock
            // is the node's, so it carries across the two.
            const auto shownAt = [&frames](double seconds) {
                Rtx::SceneDesc scene;
                SceneExtractor extractor(scene);
                extractor.setSimulationTime(seconds);
                extractor.extract(*frames, osg::Matrixf::identity(), 0);

                std::vector<osg::Vec3f> placed;
                for (const MeshInstance& instance : scene.getInstances())
                    placed.push_back(osg::Vec3f(0.0f, 0.0f, 0.0f) * instance.mTransform);

                return placed;
            };

            EXPECT_EQ(shownAt(0.0), std::vector<osg::Vec3f>{ osg::Vec3f(10.0f, 0.0f, 0.0f) })
                << "the first frame alone, and not both of them";
            EXPECT_EQ(shownAt(1.5), std::vector<osg::Vec3f>{ osg::Vec3f(0.0f, 20.0f, 0.0f) })
                << "and a second later the clock has moved on to the other";
        }

        /// A walk leaves out whatever the loader was told a hidden node carries.
        ///
        /// **The bit is asked of the loader that stamps it**, rather than named a second time here
        /// where the two could disagree — the same question `Terrain::ObjectPaging` asks to decide
        /// what distant land may copy. A host that answered nothing walks with a mask of all ones
        /// and reaches nodes the content said are not there.
        ///
        /// The same graph twice under two answers, because a mask taken from the wrong place still
        /// skips a node whose own mask is zero. The marked node here carries a real bit, so only a
        /// walk that took the loader's answer can tell the two runs apart.
        TEST_F(RtxSceneExtractorTest, aWalkLeavesOutWhatTheLoaderSaysAHiddenNodeCarries)
        {
            constexpr osg::Node::NodeMask marked = 1u << 3;
            constexpr osg::Node::NodeMask elsewhere = 1u << 4;

            const auto instancesWhenHiddenIs = [](unsigned int hiddenNodeMask) {
                const ConfiguredLoader told(hiddenNodeMask);

                osg::ref_ptr<osg::Group> quiet = new osg::Group;
                quiet->setNodeMask(marked);
                quiet->addChild(makeQuad());

                osg::ref_ptr<osg::Group> root = new osg::Group;
                root->addChild(quiet);
                root->addChild(makeQuad());

                Rtx::SceneDesc scene;
                SceneExtractor extractor(scene);
                return extractor.extract(*root, osg::Matrixf::identity(), 0).mInstances;
            };

            EXPECT_EQ(instancesWhenHiddenIs(marked), 1u) << "the marked subtree is not in the picture";
            EXPECT_EQ(instancesWhenHiddenIs(elsewhere), 2u) << "and it is, where the loader named another bit";
        }

        /// The same geometry under two parents is one mesh and two placements. Getting this wrong is
        /// not a cosmetic waste: OpenMW's resource cache hands out the same object for every
        /// reference to a model, so a cell of a hundred identical crates would build a hundred
        /// acceleration structures.
        TEST_F(RtxSceneExtractorTest, sharedGeometryIsOneMeshPlacedTwice)
        {
            osg::ref_ptr<osg::Geometry> quad = makeQuad();

            osg::ref_ptr<osg::MatrixTransform> left = new osg::MatrixTransform(osg::Matrix::translate(10.0, 0.0, 0.0));
            left->addChild(quad);
            osg::ref_ptr<osg::MatrixTransform> right = new osg::MatrixTransform(osg::Matrix::translate(0.0, 20.0, 0.0));
            right->addChild(quad);

            osg::ref_ptr<osg::Group> root = new osg::Group;
            root->addChild(left);
            root->addChild(right);

            const ExtractionStats stats = walk(*root);

            EXPECT_EQ(stats.mMeshesAdded, 1u);
            EXPECT_EQ(stats.mMeshesReused, 1u);
            ASSERT_EQ(mScene.getInstances().size(), 2u);
            EXPECT_EQ(mScene.getInstances()[0].mMesh, mScene.getInstances()[1].mMesh);

            EXPECT_EQ(placedAt(mScene, 0), osg::Vec3f(10.0f, 0.0f, 0.0f));
            EXPECT_EQ(placedAt(mScene, 1), osg::Vec3f(0.0f, 20.0f, 0.0f));
        }

        /// **A branch a switch has turned off is not in the picture, and `osg::Switch::traverse`
        /// does not say so** — under `TRAVERSE_ALL_CHILDREN` it visits every child it has. Left
        /// alone, `MWRender`'s `DayNightCallback` traces the night lamp at noon and the day mesh at
        /// midnight at the same time, and a harvested plant is traced through the unharvested one it
        /// replaced.
        TEST_F(RtxSceneExtractorTest, onlyTheBranchASwitchHasOnIsMirrored)
        {
            osg::ref_ptr<osg::MatrixTransform> day = new osg::MatrixTransform(osg::Matrix::translate(10.0, 0.0, 0.0));
            day->addChild(makeQuad());
            osg::ref_ptr<osg::MatrixTransform> night = new osg::MatrixTransform(osg::Matrix::translate(0.0, 20.0, 0.0));
            night->addChild(makeQuad());

            osg::ref_ptr<osg::Switch> root = new osg::Switch;
            root->addChild(day);
            root->addChild(night);
            root->setSingleChildOn(0);

            const ExtractionStats noon = walk(*root);
            ASSERT_TRUE(mExtractor.retire().empty()) << "the first walk swept something it had just placed";

            // The night branch was not walked, so it is not a mesh either: one added rather than two.
            EXPECT_EQ(noon.mMeshesAdded, 1u);
            EXPECT_EQ(noon.mInstances, 1u);
            ASSERT_EQ(mScene.getInstances().size(), 1u);
            EXPECT_EQ(placedAt(mScene, 0), osg::Vec3f(10.0f, 0.0f, 0.0f));

            root->setSingleChildOn(1);
            mScene.clearPlacement();
            const ExtractionStats midnight = walk(*root, 1);

            EXPECT_EQ(midnight.mMeshesAdded, 1u) << "the branch that came on had never been read";
            EXPECT_EQ(midnight.mMeshesReused, 0u);
            EXPECT_EQ(midnight.mInstances, 1u);

            // The branch that went off is standing until the sweep, which is what any placement
            // leaving the graph costs — and gone after it, in its own slot rather than by
            // renumbering the one that arrived.
            EXPECT_EQ(mExtractor.retire().mMeshes, 1u);
            EXPECT_EQ(mScene.getPlacedCount(), 1u);
            ASSERT_EQ(mScene.getInstances().size(), 2u);
            EXPECT_FALSE(mScene.getInstances()[0].isPlaced()) << "the day branch outlived the sweep";
            ASSERT_TRUE(mScene.getInstances()[1].isPlaced());
            EXPECT_EQ(placedAt(mScene, 1), osg::Vec3f(0.0f, 20.0f, 0.0f));
        }

        TEST_F(RtxSceneExtractorTest, theRootTransformIsAppliedAfterTheGraphsOwn)
        {
            osg::ref_ptr<osg::MatrixTransform> inner = new osg::MatrixTransform(osg::Matrix::scale(2.0, 2.0, 2.0));
            inner->addChild(makeQuad());

            mExtractor.extract(*inner, osg::Matrixf::translate(0.0f, 0.0f, 5.0f), 0);

            // The quad's (1,1,0) corner doubles to (2,2,0), then rises by five.
            ASSERT_EQ(mScene.getInstances().size(), 1u);
            EXPECT_EQ(osg::Vec3f(1.0f, 1.0f, 0.0f) * mScene.getInstances()[0].mTransform, osg::Vec3f(2.0f, 2.0f, 5.0f));
        }

        /// The visitor accumulates the local-to-world on its way down instead of rebuilding each
        /// drawable's chain from the root, so what a chain composes to is its own property to hold.
        TEST_F(RtxSceneExtractorTest, nestedTransformsComposeFromTheRootDownwards)
        {
            // Outermost first: scale by two, then rotate a quarter turn about z, then move along x.
            osg::ref_ptr<osg::MatrixTransform> scale = new osg::MatrixTransform(osg::Matrix::scale(2.0, 2.0, 2.0));
            osg::ref_ptr<osg::MatrixTransform> turn
                = new osg::MatrixTransform(osg::Matrix::rotate(osg::PI_2, osg::Vec3d(0.0, 0.0, 1.0)));
            osg::ref_ptr<osg::MatrixTransform> shift = new osg::MatrixTransform(osg::Matrix::translate(3.0, 0.0, 0.0));

            scale->addChild(turn);
            turn->addChild(shift);
            shift->addChild(makeQuad());

            walk(*scale);

            ASSERT_EQ(mScene.getInstances().size(), 1u);
            const osg::Matrixf& place = mScene.getInstances()[0].mTransform;

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

        TEST_F(RtxSceneExtractorTest, aTransformThatReadsTheVisitorIsGivenOne)
        {
            osg::ref_ptr<VisitorReadingTransform> reads = new VisitorReadingTransform;
            reads->setMatrix(osg::Matrix::translate(0.0, 0.0, 4.0));
            reads->addChild(makeQuad());

            walk(*reads);

            EXPECT_EQ(reads->mSaw, osg::NodeVisitor::NODE_VISITOR) << "the transform was handed a null visitor";

            // And it still placed what was under it, at the transform it asked for.
            ASSERT_EQ(mScene.getPlacedCount(), 1u);
            EXPECT_EQ(placedAt(mScene, 0), osg::Vec3f(0.0f, 0.0f, 4.0f));
        }

        /// An absolute reference frame replaces what is above it rather than adding to it, which is
        /// a branch inside `computeLocalToWorldMatrix` and the one thing an accumulating visitor
        /// could quietly get wrong by adding where it should overwrite.
        TEST_F(RtxSceneExtractorTest, anAbsoluteFrameDiscardsTheTransformsAboveIt)
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

            walk(*above);

            ASSERT_EQ(mScene.getInstances().size(), 2u);

            // The absolute one stands at its own translation and nowhere near the hundred above it.
            EXPECT_EQ(placedAt(mScene, 0), osg::Vec3f(0.0f, 0.0f, 7.0f));

            // The relative one carries it.
            EXPECT_EQ(placedAt(mScene, 1), osg::Vec3f(100.0f, 100.0f, 107.0f));
        }

        /// The property the incremental mirror rests on: nothing changed, so nothing is added.
        TEST_F(RtxSceneExtractorTest, aSecondPassOverAnUnchangedGraphAddsNothing)
        {
            osg::ref_ptr<osg::Group> root = new osg::Group;
            root->addChild(makeQuad());
            root->addChild(makeQuad());

            const ExtractionStats first = walk(*root);

            const ExtractionStats second = walk(*root);

            EXPECT_EQ(second.mMeshesAdded, 0u);
            EXPECT_EQ(second.mMeshesReused, 2u);
            EXPECT_EQ(mScene.getMeshes().size(), 2u);

            // **Each walk counts into its own report and never into the walk before it.** What a
            // resolver counts through is `MirrorPass`, which is the mirror's own member and outlives
            // the call — so a walk that left it pointed where the last one did would report the sum
            // here and leave the first standing at a number it never met.
            EXPECT_EQ(first.mMeshesAdded, 2u);
            EXPECT_EQ(first.mMeshesReused, 0u);

            // **The property the incremental mirror rests on, in its strongest form.** The same mesh
            // at two places is still two rows of the acceleration structure — placements are not
            // deduplicated — but a second pass over an unchanged graph finds the slots those two
            // already hold rather than making two more. Nothing was added, and nothing moved.
            EXPECT_EQ(mScene.getPlacedCount(), 2u);
            EXPECT_EQ(mScene.getInstances().size(), 2u);

            mScene.advancePlacement();
            EXPECT_EQ(walk(*root).mInstances, 2u);
            EXPECT_TRUE(mScene.getMoved().empty()) << "an unchanged graph reported a placement moving";
        }

        /// **A node path does not identify a placement, and this is the case that proves it.**
        /// `SceneManager::getTemplate` hands out one node per model, so every reference to that model
        /// is walked from the same node down the same path. Without the anchor they share a slot,
        /// and a hundred crates collapse into one.
        TEST_F(RtxSceneExtractorTest, oneTemplateWalkedUnderTwoAnchorsIsTwoPlacements)
        {
            osg::ref_ptr<osg::Group> shared = new osg::Group;
            shared->addChild(makeQuad());

            mExtractor.extract(*shared, osg::Matrixf::translate(10.0f, 0.0f, 0.0f), 1);
            mExtractor.extract(*shared, osg::Matrixf::translate(0.0f, 20.0f, 0.0f), 2);

            ASSERT_EQ(mScene.getPlacedCount(), 2u);
            EXPECT_EQ(mScene.getMeshes().size(), 1u) << "one model is still one mesh";

            EXPECT_EQ(placedAt(mScene, 0), osg::Vec3f(10.0f, 0.0f, 0.0f));
            EXPECT_EQ(placedAt(mScene, 1), osg::Vec3f(0.0f, 20.0f, 0.0f));

            // And they keep their own histories. Moving one must leave the other reporting nothing —
            // sharing a slot would have the still one inherit the mover's previous transform and
            // smear across the frame.
            mScene.advancePlacement();
            mExtractor.extract(*shared, osg::Matrixf::translate(11.0f, 0.0f, 0.0f), 1);
            mExtractor.extract(*shared, osg::Matrixf::translate(0.0f, 20.0f, 0.0f), 2);

            ASSERT_EQ(mScene.getMoved().size(), 1u);
            EXPECT_EQ(mScene.getMoved()[0], 0u);
        }
    }
}
