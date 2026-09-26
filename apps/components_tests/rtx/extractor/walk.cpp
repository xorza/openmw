#include <cstddef>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <osg/Array>
#include <osg/LOD>
#include <osg/Math>
#include <osg/Matrix>
#include <osg/MatrixTransform>
#include <osg/Matrixd>
#include <osg/Matrixf>
#include <osg/NodeVisitor>
#include <osg/Sequence>
#include <osg/Switch>
#include <osg/Transform>
#include <osg/Vec2f>
#include <osg/Vec3d>
#include <osg/Vec3f>
#include <osg/ref_ptr>

#include <components/nif/niftypes.hpp>
#include <components/nifosg/autotransform.hpp>
#include <components/rtx/camera.hpp>
#include <components/rtx/mesh.hpp>
#include <components/rtx/meshtable.hpp>
#include <components/rtx/refusals.hpp>
#include <components/rtx/sceneextractor.hpp>

#include "fixture.hpp"

namespace Rtx::Testing
{
    namespace
    {
        TEST_F(RtxSceneExtractorTest, twoDrawablesBecomeTwoMeshesAndTwoInstances)
        {
            osg::ref_ptr<osg::Group> root = new osg::Group;
            root->addChild(makeQuad());
            root->addChild(makeQuad());

            const ExtractionStats stats = walk(*root);

            EXPECT_EQ(stats.mMeshesAdded, 2u);
            EXPECT_EQ(stats.mMeshesReused, 0u);
            EXPECT_EQ(stats.mInstances, 2u);
            EXPECT_EQ(mScene.meshes().getTriangleCount(), 4u);
        }

        /// **What a content file describes and this renderer cannot take is refused per drawable,
        /// once, and the walk goes on.** The quad beside the refused mesh stands. Each is refused to
        /// the scene once, however many walks meet it, and the walk after reads the drawable no
        /// more. A mesh past one block is one such, and a triangle naming a vertex its drawable does
        /// not have is another, which read on would have been a read past the positions and a
        /// fault on the device.
        TEST_F(RtxSceneExtractorTest, aMeshThisCannotBuildIsRefusedOnceAndTheWalkGoesOn)
        {
            osg::ref_ptr<osg::Group> root = new osg::Group;
            root->addChild(makePastOneBlock());
            root->addChild(makeIndexPastItsVertices());
            root->addChild(makeQuad());

            const ExtractionStats first = walk(*root);
            EXPECT_EQ(mScene.refusals().count(Refused::Mesh), 2u);
            EXPECT_EQ(first.mMeshesAdded, 1u);
            EXPECT_EQ(first.mInstances, 1u);
            mExtractor.retire();

            const ExtractionStats second = walk(*root);
            EXPECT_EQ(mScene.refusals().count(Refused::Mesh), 2u) << "a refused drawable is refused once";
            EXPECT_EQ(second.mMeshesAdded, 0u);
            EXPECT_EQ(second.mMeshesReused, 1u);
            EXPECT_EQ(second.mInstances, 1u);
            mExtractor.retire();

            EXPECT_EQ(mScene.meshes().getLiveCount(), 1u)
                << "the refusal names no row, and the sweep keeps none for it";
            EXPECT_TRUE(mScene.isConsistent());
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
                for (const PlacementRow& row : scene.placements().getRows())
                    placed.push_back(osg::Vec3f(0.0f, 0.0f, 0.0f) * row.mInstance.mTransform);

                return placed;
            };

            EXPECT_EQ(shownAt(0.0), std::vector<osg::Vec3f>{ osg::Vec3f(10.0f, 0.0f, 0.0f) })
                << "the first frame alone, and not both of them";
            EXPECT_EQ(shownAt(1.5), std::vector<osg::Vec3f>{ osg::Vec3f(0.0f, 20.0f, 0.0f) })
                << "and a second later the clock has moved on to the other";
        }

        /// A walk leaves out a subtree the mask it was given excludes, and reaches every node until
        /// it is given one.
        ///
        /// **The owner states the mask and this component never derives it.** The bit a hidden node
        /// carries is the engine's own — `MWRender::Mask_UpdateVisitor` — which this side cannot
        /// name, and a default that asked `NifOsg::Loader` for it answered with whatever the loader
        /// has been told by the moment the extractor is built. In the game it has been told
        /// nothing: the renderer is built before the rendering manager that tells it, so every node
        /// a `NifOsg::VisController` hides is walked, placed and traced.
        ///
        /// The marked node carries a real bit rather than none, so only a walk that honours the
        /// mask it was handed can tell the three runs apart.
        TEST_F(RtxSceneExtractorTest, aWalkLeavesOutWhatTheMaskItWasGivenExcludes)
        {
            constexpr osg::Node::NodeMask marked = 1u << 3;

            const auto instancesUnder = [](std::optional<osg::Node::NodeMask> mask) {
                osg::ref_ptr<osg::Group> quiet = new osg::Group;
                quiet->setNodeMask(marked);
                quiet->addChild(makeQuad());

                osg::ref_ptr<osg::Group> root = new osg::Group;
                root->addChild(quiet);
                root->addChild(makeQuad());

                Rtx::SceneDesc scene;
                SceneExtractor extractor(scene);
                if (mask.has_value())
                    extractor.setTraversalMask(*mask);

                return extractor.extract(*root, osg::Matrixf::identity(), 0).mInstances;
            };

            EXPECT_EQ(instancesUnder(~marked), 1u) << "the marked subtree is not in the picture";
            EXPECT_EQ(instancesUnder(~0u), 2u) << "and it is, under a mask that excludes nothing";
            EXPECT_EQ(instancesUnder(std::nullopt), 2u) << "which is what an extractor nobody told walks with";
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
            const std::span<const Rtx::PlacementRow> placed = mScene.placements().getRows();
            ASSERT_EQ(placed.size(), 2u);
            EXPECT_EQ(placed[0].mInstance.mMesh, placed[1].mInstance.mMesh);

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
            ASSERT_EQ(mScene.placements().getRows().size(), 1u);
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
            EXPECT_EQ(mScene.placements().getCounts().mPlaced, 1u);
            ASSERT_EQ(mScene.placements().getRows().size(), 2u);
            EXPECT_FALSE(mScene.placements().getRows()[0].mInstance.isPlaced()) << "the day branch outlived the sweep";
            ASSERT_TRUE(mScene.placements().getRows()[1].mInstance.isPlaced());
            EXPECT_EQ(placedAt(mScene, 1), osg::Vec3f(0.0f, 20.0f, 0.0f));
        }

        /// **An LOD stands its nearest level and nothing else, wherever the eye is**, and
        /// `osg::LOD::traverse` does not say so either: under `TRAVERSE_ALL_CHILDREN` every level is
        /// visited, and a `NiLODNode` would be traced with all of its budgets standing at once. The
        /// level is the one whose range starts nearest, whatever order the loader left them in.
        TEST_F(RtxSceneExtractorTest, onlyTheNearestLevelOfAnLodIsMirrored)
        {
            osg::ref_ptr<osg::MatrixTransform> far = new osg::MatrixTransform(osg::Matrix::translate(10.0, 0.0, 0.0));
            far->addChild(makeQuad());
            osg::ref_ptr<osg::MatrixTransform> near = new osg::MatrixTransform(osg::Matrix::translate(0.0, 20.0, 0.0));
            near->addChild(makeQuad());

            // The far level first, so the answer cannot be the first child.
            osg::ref_ptr<osg::LOD> root = new osg::LOD;
            root->addChild(far, 500.0f, 4000.0f);
            root->addChild(near, 0.0f, 500.0f);

            const ExtractionStats stats = walk(*root);
            EXPECT_EQ(stats.mMeshesAdded, 1u) << "the far level was read";
            EXPECT_EQ(stats.mInstances, 1u);
            ASSERT_EQ(mScene.placements().getRows().size(), 1u);
            EXPECT_EQ(placedAt(mScene, 0), osg::Vec3f(0.0f, 20.0f, 0.0f)) << "the far level stands";
        }

        TEST_F(RtxSceneExtractorTest, theRootTransformIsAppliedAfterTheGraphsOwn)
        {
            osg::ref_ptr<osg::MatrixTransform> inner = new osg::MatrixTransform(osg::Matrix::scale(2.0, 2.0, 2.0));
            inner->addChild(makeQuad());

            mExtractor.extract(*inner, osg::Matrixf::translate(0.0f, 0.0f, 5.0f), 0);

            // The quad's (1,1,0) corner doubles to (2,2,0), then rises by five.
            ASSERT_EQ(mScene.placements().getRows().size(), 1u);
            EXPECT_EQ(osg::Vec3f(1.0f, 1.0f, 0.0f) * mScene.placements().getRows()[0].mInstance.mTransform,
                osg::Vec3f(2.0f, 2.0f, 5.0f));
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

            ASSERT_EQ(mScene.placements().getRows().size(), 1u);
            const osg::Matrixf& place = mScene.placements().getRows()[0].mInstance.mTransform;

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
            ASSERT_EQ(mScene.placements().getCounts().mPlaced, 1u);
            EXPECT_EQ(placedAt(mScene, 0), osg::Vec3f(0.0f, 0.0f, 4.0f));
        }

        /// A billboard turns toward the eye the walk was told, and stays where it stands under a
        /// walk told none.
        ///
        /// **`NifOsg::AutoTransform` turns only under a cull visitor**, and this walk is none: handed
        /// itself it keeps the rotation a cull last left, which here is the one the file was
        /// authored with. `RigidFaceCamera` maps the node's own +Z onto the look and its own +Y
        /// onto the up, so what is asserted is the placed axes against the eye handed in, under two
        /// eyes and under a parent that moves the node — the eye is taken into the node's own frame
        /// first.
        TEST_F(RtxSceneExtractorTest, aBillboardTurnsTowardTheEyeTheWalkWasTold)
        {
            // Built as the loader builds one, from a record: an `AutoTransform` made from nothing
            // has a scale of nought and collapses whatever is under it.
            Nif::NiTransform authored;
            authored.mTranslation = osg::Vec3f();
            authored.mScale = 1.0f;
            osg::ref_ptr<NifOsg::AutoTransform> billboard
                = new NifOsg::AutoTransform(authored, NifOsg::AutoTransform::Mode::RigidFaceCamera);
            billboard->addChild(makeQuad());

            // Under a parent turned a quarter about z, so the eye is not the node's own frame.
            osg::ref_ptr<osg::MatrixTransform> parent
                = new osg::MatrixTransform(osg::Matrix::rotate(osg::PI_2, osg::Vec3d(0.0, 0.0, 1.0)));
            parent->addChild(billboard);

            const auto axesUnder = [&](const std::optional<ViewBasis>& eye, std::size_t frame) {
                mExtractor.setEye(eye);
                mScene.clearPlacement();
                walk(*parent, 0, frame);
                EXPECT_EQ(mScene.placements().getCounts().mPlaced, 1u);
                const osg::Matrixf& place = mScene.placements().getRows()[0].mInstance.mTransform;
                return std::pair(osg::Matrixf::transform3x3(osg::Vec3f(0.0f, 0.0f, 1.0f), place),
                    osg::Matrixf::transform3x3(osg::Vec3f(0.0f, 1.0f, 0.0f), place));
            };

            const auto expectAxis = [](const osg::Vec3f& axis, const osg::Vec3f& wanted, const char* what) {
                EXPECT_NEAR(axis.x(), wanted.x(), 1e-5f) << what;
                EXPECT_NEAR(axis.y(), wanted.y(), 1e-5f) << what;
                EXPECT_NEAR(axis.z(), wanted.z(), 1e-5f) << what;
            };

            // No eye: the parent's quarter turn and nothing else, so the node's z stays z and its y
            // turns to -x.
            const auto [stillZ, stillY] = axesUnder(std::nullopt, 1);
            expectAxis(stillZ, osg::Vec3f(0.0f, 0.0f, 1.0f), "z under no eye");
            expectAxis(stillY, osg::Vec3f(-1.0f, 0.0f, 0.0f), "y under no eye");

            // An eye looking along +y with z up: the node's z lands on the look and its y on the up,
            // whatever the parent did.
            const auto [towardY, upZ] = axesUnder(
                ViewBasis{ .mOrigin = osg::Vec3f(0.0f, -100.0f, 0.0f), .mForward = osg::Vec3f(0.0f, 1.0f, 0.0f) }, 2);
            expectAxis(towardY, osg::Vec3f(0.0f, 1.0f, 0.0f), "z toward an eye looking along y");
            expectAxis(upZ, osg::Vec3f(0.0f, 0.0f, 1.0f), "y up for an eye looking along y");

            // And an eye looking along -x turns it the other way.
            const auto [towardX, upStill] = axesUnder(
                ViewBasis{ .mOrigin = osg::Vec3f(100.0f, 0.0f, 0.0f), .mForward = osg::Vec3f(-1.0f, 0.0f, 0.0f) }, 3);
            expectAxis(towardX, osg::Vec3f(-1.0f, 0.0f, 0.0f), "z toward an eye looking along -x");
            expectAxis(upStill, osg::Vec3f(0.0f, 0.0f, 1.0f), "y up for an eye looking along -x");
        }

        /// The basis a view matrix stands is its inverse's translation, its own +X, -Z and +Y.
        TEST(RtxViewBasisTest, aBasisIsReadOffTheInverseOfAViewMatrix)
        {
            const osg::Matrixd view = osg::Matrixd::lookAt(
                osg::Vec3d(10.0, -50.0, 5.0), osg::Vec3d(10.0, 50.0, 5.0), osg::Vec3d(0.0, 0.0, 1.0));
            const ViewBasis eye = viewBasisOf(osg::Matrixd::inverse(view)).value();

            EXPECT_NEAR(eye.mOrigin.x(), 10.0f, 1e-4f);
            EXPECT_NEAR(eye.mOrigin.y(), -50.0f, 1e-4f);
            EXPECT_NEAR(eye.mOrigin.z(), 5.0f, 1e-4f);
            EXPECT_NEAR(eye.mForward.x(), 0.0f, 1e-5f);
            EXPECT_NEAR(eye.mForward.y(), 1.0f, 1e-5f);
            EXPECT_NEAR(eye.mForward.z(), 0.0f, 1e-5f);
            EXPECT_NEAR(eye.mRight.x(), 1.0f, 1e-5f);
            EXPECT_NEAR(eye.mRight.y(), 0.0f, 1e-5f);
            EXPECT_NEAR(eye.mRight.z(), 0.0f, 1e-5f);
            EXPECT_NEAR(eye.mUp.x(), 0.0f, 1e-5f);
            EXPECT_NEAR(eye.mUp.y(), 0.0f, 1e-5f);
            EXPECT_NEAR(eye.mUp.z(), 1.0f, 1e-5f);
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

            ASSERT_EQ(mScene.placements().getRows().size(), 2u);

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
            EXPECT_EQ(mScene.meshes().getRows().size(), 2u);

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
            EXPECT_EQ(mScene.placements().getCounts().mPlaced, 2u);
            EXPECT_EQ(mScene.placements().getRows().size(), 2u);

            mScene.placements().advance();
            EXPECT_EQ(walk(*root).mInstances, 2u);
            EXPECT_TRUE(mScene.placements().getMoved().empty()) << "an unchanged graph reported a placement moving";
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

            ASSERT_EQ(mScene.placements().getCounts().mPlaced, 2u);
            EXPECT_EQ(mScene.meshes().getRows().size(), 1u) << "one model is still one mesh";

            EXPECT_EQ(placedAt(mScene, 0), osg::Vec3f(10.0f, 0.0f, 0.0f));
            EXPECT_EQ(placedAt(mScene, 1), osg::Vec3f(0.0f, 20.0f, 0.0f));

            // And they keep their own histories. Moving one must leave the other reporting nothing —
            // sharing a slot would have the still one inherit the mover's previous transform and
            // smear across the frame.
            mScene.placements().advance();
            mExtractor.extract(*shared, osg::Matrixf::translate(11.0f, 0.0f, 0.0f), 1);
            mExtractor.extract(*shared, osg::Matrixf::translate(0.0f, 20.0f, 0.0f), 2);

            ASSERT_EQ(mScene.placements().getMoved().size(), 1u);
            EXPECT_EQ(mScene.placements().getMoved()[0], 0u);
        }

        /// **Texture coordinates are read off the array's own type byte**, which is what
        /// `asVec2Array` asks and what a `dynamic_cast` walks the class hierarchy to answer. The
        /// two agree on a `Vec2Array` and on nothing else, so a three-component array is not
        /// coordinates this reads, and the mesh that brought them is refused rather than drawn as
        /// if it brought none.
        ///
        /// Hand-written: the second corner's V is a half.
        TEST_F(RtxSceneExtractorTest, textureCoordinatesAreReadOnlyWhereTheArrayIsAPairPerVertex)
        {
            osg::ref_ptr<osg::Vec2Array> pairs = new osg::Vec2Array;
            for (const osg::Vec2f& value :
                { osg::Vec2f(0.0f, 0.0f), osg::Vec2f(1.0f, 0.5f), osg::Vec2f(1.0f, 1.0f), osg::Vec2f(0.0f, 1.0f) })
                pairs->push_back(value);

            osg::ref_ptr<osg::Geometry> read = makeQuad();
            read->setTexCoordArray(0, pairs);

            osg::ref_ptr<osg::Geometry> refused = makeQuad();
            refused->setTexCoordArray(0,
                makePositions({ osg::Vec3f(0.0f, 0.0f, 0.0f), osg::Vec3f(1.0f, 0.5f, 0.0f),
                    osg::Vec3f(1.0f, 1.0f, 0.0f), osg::Vec3f(0.0f, 1.0f, 0.0f) }));

            osg::ref_ptr<osg::Group> root = new osg::Group;
            root->addChild(read);
            root->addChild(refused);

            mExtractor.extract(*root, osg::Matrixf::identity(), 1);

            const Rtx::MeshTable& meshes = mScene.meshes();
            ASSERT_EQ(meshes.getRows().size(), 1u);
            const std::span<const osg::Vec2f> coords = meshes.getRows()[0].mVertices.in(meshes.getTexCoords());
            ASSERT_EQ(coords.size(), 4u);
            EXPECT_EQ(coords[1], osg::Vec2f(1.0f, 0.5f)) << "a pair per vertex is read";
            EXPECT_EQ(mScene.refusals().count(Refused::Mesh), 1u) << "three components are no texture coordinate";
        }
    }
}
