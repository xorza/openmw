#include <gtest/gtest.h>

#include <osg/Camera>
#include <osg/FrameStamp>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/Matrix>
#include <osg/MatrixTransform>
#include <osg/Matrixd>
#include <osg/Node>
#include <osg/Vec3f>
#include <osg/ref_ptr>

#include <apps/components_tests/rtx/extractor/fixture.hpp>
#include <apps/components_tests/rtx/support/fakeland.hpp>
#include <apps/openmw/mwrender/objectstorage.hpp>
#include <apps/openmw/mwrender/precipitation.hpp>
#include <apps/openmw/mwrender/rtx/tracedterrain.hpp>
#include <apps/openmw/mwrender/rtx/worldmirror.hpp>
#include <apps/openmw/mwrender/sceneframe.hpp>
#include <apps/openmw/mwrender/skystate.hpp>
#include <apps/openmw/mwrender/vismask.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/nifosg/nifloader.hpp>
#include <components/resource/bgsmfilemanager.hpp>
#include <components/resource/imagemanager.hpp>
#include <components/resource/niffilemanager.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/rtx/cellworld.hpp>
#include <components/rtx/extractionstats.hpp>
#include <components/vfs/manager.hpp>

namespace MWRender
{
    namespace
    {
        /// A frame with two unstamped bodies in it and nothing else: no weather, no water, no
        /// cells for the ring — the mirror is never attached, so the ring's world is unreadable —
        /// and the eye at the origin. Unstamped, so each is known by its place among its
        /// siblings, which is the one way a body's identity can move while the body stays.
        struct TwoBodyFrame
        {
            VFS::Manager mVfs;
            Resource::ImageManager mImages{ &mVfs, 0 };
            Resource::NifFileManager mNifs{ &mVfs, nullptr };
            Resource::BgsmFileManager mMaterials{ &mVfs, 0 };
            Resource::SceneManager mScenes{ &mVfs, &mImages, &mNifs, &mMaterials, 0 };

            osg::ref_ptr<osg::Group> mRoot = new osg::Group;
            osg::ref_ptr<osg::MatrixTransform> mFirst = new osg::MatrixTransform(osg::Matrix::translate(1.0, 0.0, 0.0));
            osg::ref_ptr<osg::MatrixTransform> mSecond
                = new osg::MatrixTransform(osg::Matrix::translate(5.0, 0.0, 0.0));

            osg::ref_ptr<osg::Group> mSkyRoot = new osg::Group;
            osg::ref_ptr<osg::Camera> mCamera = new osg::Camera;
            Precipitation mPrecipitation{ mSkyRoot, mCamera, &mScenes };

            Rtx::Testing::FakeLand mLand;
            osg::ref_ptr<osg::Group> mGroundRoot = new osg::Group;
            TracedTerrain mTerrain{ *mGroundRoot, mLand, Mask_Terrain, ESM::Cell::sDefaultWorldspaceId };
            ObjectStorage mObjects;

            osg::FrameStamp mWhen;
            SkyState mSky;
            WorldState mWorld;
            EyeState mEye;

            TwoBodyFrame()
            {
                osg::ref_ptr<osg::Geometry> shared = Rtx::Testing::makeQuad();
                mFirst->addChild(shared);
                mSecond->addChild(shared);
                mRoot->addChild(mFirst);
                mRoot->addChild(mSecond);
            }

            SceneFrame frame()
            {
                return SceneFrame{
                    .mScene = *mRoot,
                    .mWhen = mWhen,
                    .mSky = mSky,
                    .mPrecipitation = mPrecipitation,
                    .mWorld = mWorld,
                    .mEye = mEye,
                    .mTerrain = mTerrain,
                    .mObjectStorage = mObjects,
                };
            }
        };

        /// **What the walk stopped finding is gone from the scene the hand-over takes.** A sweep run
        /// after the frame would trace a slot the walk no longer met once more where it last stood:
        /// the player's body, stamped afresh on every cell it entered, would draw as a double of
        /// itself one frame behind — the arms in front of a player walking backward. A stamp is for
        /// a node's life, so the identity that moves here is a structural one: the
        /// second body shifts into the first's place when the first goes, and is walked as the
        /// first, moved. What must not stand beside it is its own old slot, where the last frame
        /// left it.
        TEST(RtxWorldMirrorTest, whatTheWalkStoppedFindingIsGoneBeforeTheHandOver)
        {
            TwoBodyFrame world;
            WorldMirror mirror(Rtx::MirrorKnobs{});
            const osg::Matrixd view = osg::Matrixd::identity();

            const Rtx::ExtractionStats both = mirror.mirror(world.frame(), view, 1);
            ASSERT_EQ(both.mInstances, 2u);
            ASSERT_EQ(mirror.getScene().placements().getCounts().mPlaced, 2u);
            ASSERT_EQ(Rtx::Testing::placedAt(mirror.getScene(), 0), osg::Vec3f(1.0f, 0.0f, 0.0f));
            ASSERT_EQ(Rtx::Testing::placedAt(mirror.getScene(), 1), osg::Vec3f(5.0f, 0.0f, 0.0f));

            // The first goes and the second moves on: one placement, in the first's slot, standing
            // where the second is now — and not that beside the second's old slot at where it was.
            world.mRoot->removeChild(world.mFirst);
            world.mSecond->setMatrix(osg::Matrix::translate(9.0, 0.0, 0.0));
            const Rtx::ExtractionStats shifted = mirror.mirror(world.frame(), view, 2);
            EXPECT_EQ(shifted.mInstances, 1u);
            EXPECT_EQ(shifted.mRestood, 0u);
            EXPECT_EQ(mirror.getScene().placements().getCounts().mPlaced, 1u) << "the second's old slot still stands";
            EXPECT_EQ(Rtx::Testing::placedAt(mirror.getScene(), 0), osg::Vec3f(9.0f, 0.0f, 0.0f));
            EXPECT_FALSE(mirror.getScene().placements().getRows()[1].mInstance.isPlaced()) << "the old slot was kept";

            // And a body the graph let go of is placed nowhere on the frame it went.
            world.mRoot->removeChild(world.mSecond);
            const Rtx::ExtractionStats gone = mirror.mirror(world.frame(), view, 3);
            EXPECT_EQ(gone.mInstances, 0u);
            EXPECT_EQ(mirror.getScene().placements().getCounts().mPlaced, 0u) << "a slot traced after its body went";
        }

        /// **The player is the one thing a mirror leaves out on a question about the camera.** Every
        /// other exclusion is a fact about the subtree — the sky is drawn by the trace, the simple
        /// water is a duplicate — and this one is a fact about who is looking. A camera standing
        /// where the player stands traced a boot thirteen units from the eye.
        TEST(RtxWorldMirrorTest, thePlayerIsWalkedOnlyForACameraThatIsTheirEye)
        {
            WorldMirror mirror(Rtx::MirrorKnobs{});

            const osg::Node::NodeMask playing = mirror.getTraversalMask();
            EXPECT_NE(playing & Mask_Player, 0u) << "a game somebody is playing draws them";
            EXPECT_EQ(playing & Mask_Terrain, 0u) << "the intersector's ground is not the ring's";
            EXPECT_EQ(playing & Mask_UpdateVisitor, 0u) << "and what the content hides stays hidden";

            mirror.setShowsPlayer(false);
            const osg::Node::NodeMask watching = mirror.getTraversalMask();
            EXPECT_EQ(watching & Mask_Player, 0u);

            // **And nothing else moved with it.** The mask carries the sky, the sun, the duplicate
            // water and whatever the content hid, and a recompute that dropped one of those would
            // trace a world with no ground or draw the sea twice.
            EXPECT_EQ(watching, playing & ~static_cast<osg::Node::NodeMask>(Mask_Player));

            mirror.setShowsPlayer(true);
            EXPECT_EQ(mirror.getTraversalMask(), playing) << "and it comes back";
        }

        /// **What the content hides is left out whether or not the loader has been told which bit
        /// hides.** `NifOsg::VisController` stamps `Mask_UpdateVisitor` on a node its data says is
        /// not there, and `NifOsg::Loader` learns that bit from `RenderingManager` — which the
        /// engine builds *after* the renderer. So a mask that asks the loader for it subtracts
        /// nought, and every hidden node in the game is walked, placed and traced: the Heart of
        /// Lorkhan stands wearing the whole of its destruction, each shell at the frame that
        /// sequence opens on.
        ///
        /// The loader is left untold here, which is the state the mirror is really built in.
        TEST(RtxWorldMirrorTest, whatTheContentHidesIsLeftOutBeforeTheLoaderIsToldWhichBitHides)
        {
            const unsigned int told = NifOsg::Loader::getHiddenNodeMask();
            NifOsg::Loader::setHiddenNodeMask(0);

            const osg::Node::NodeMask mask = WorldMirror(Rtx::MirrorKnobs{}).getTraversalMask();

            NifOsg::Loader::setHiddenNodeMask(told);

            EXPECT_EQ(mask & Mask_UpdateVisitor, 0u) << "a node the content hid is walked";
        }
    }
}
