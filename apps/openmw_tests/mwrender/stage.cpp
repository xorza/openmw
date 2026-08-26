#include <gtest/gtest.h>

#include <osg/Camera>
#include <osg/FrameStamp>
#include <osg/Group>
#include <osg/NodeCallback>
#include <osg/Stats>

#include <osgGA/EventQueue>

#include <osgUtil/UpdateVisitor>

#include "apps/openmw/mwrender/stage.hpp"

namespace MWRender
{
    namespace
    {
        /// Counts the traversals that reached the node it hangs on.
        class CountingCallback : public osg::NodeCallback
        {
        public:
            void operator()(osg::Node* node, osg::NodeVisitor* visitor) override
            {
                ++mReached;
                traverse(node, visitor);
            }

            unsigned int mReached = 0;
        };

        /// A stage with everything a renderer would have adopted into it.
        struct Fixture
        {
            osg::ref_ptr<osg::Camera> mCamera = new osg::Camera;
            osg::ref_ptr<osg::FrameStamp> mFrameStamp = new osg::FrameStamp;
            osg::ref_ptr<osgGA::EventQueue> mEvents = new osgGA::EventQueue;
            osg::ref_ptr<osgUtil::UpdateVisitor> mUpdateVisitor = new osgUtil::UpdateVisitor;
            osg::ref_ptr<osg::Stats> mStats = new osg::Stats("test");
            osg::ref_ptr<osg::Group> mSceneRoot = new osg::Group;

            osg::ref_ptr<CountingCallback> mEye = new CountingCallback;
            osg::ref_ptr<CountingCallback> mWorld = new CountingCallback;

            Stage mStage;

            Fixture()
            {
                mCamera->addUpdateCallback(mEye);
                mSceneRoot->addUpdateCallback(mWorld);

                mStage.adopt(*mCamera, *mFrameStamp, *mEvents, *mUpdateVisitor, *mStats);
                mStage.setSceneRoot(*mSceneRoot);
            }
        };

        /// **The world hangs off the camera, and that is what lets the player touch it.**
        /// `RenderingManager::castCameraToViewportRay` accepts an intersection visitor on the camera
        /// because its ray is in projection coordinates and the camera's matrices are what put that
        /// ray in the world. A camera with no children answers "nothing" to every activation in the
        /// game.
        TEST(MWRenderStageTest, theWorldHangsOffTheCameraAndIsParentedThereOnlyOnce)
        {
            Fixture fixture;

            ASSERT_EQ(fixture.mCamera->getNumChildren(), 1u);
            EXPECT_EQ(fixture.mCamera->getChild(0), fixture.mSceneRoot.get());

            // The rasterizer parents the same root a second time through `osgViewer::Viewer`, so
            // saying it twice has to mean what saying it once meant.
            fixture.mStage.setSceneRoot(*fixture.mSceneRoot);
            EXPECT_EQ(fixture.mCamera->getNumChildren(), 1u);

            // What picking depends on: a visitor accepted on the camera arrives at the world.
            fixture.mCamera->accept(*fixture.mUpdateVisitor);
            EXPECT_EQ(fixture.mWorld->mReached, 1u) << "a visitor on the camera never reached the world";
        }

        /// **The eye is updated without the world being walked a second time.**
        ///
        /// A renderer that drives its own frame walks the scene from its own root — for the node
        /// path, which must not start at an `ABSOLUTE_RF` camera — and then wants the one callback
        /// the camera carries. Accepting on the camera to get it ran every animation controller,
        /// every `LightController` and `LightManager::update` twice in the same frame, at the same
        /// traversal number.
        TEST(MWRenderStageTest, updatingTheEyeRunsTheCamerasCallbackAndNothingBelowIt)
        {
            Fixture fixture;

            fixture.mSceneRoot->accept(*fixture.mUpdateVisitor);
            ASSERT_EQ(fixture.mWorld->mReached, 1u);
            ASSERT_EQ(fixture.mEye->mReached, 0u) << "the camera is not below the scene root";

            const osg::NodeVisitor::TraversalMode was = fixture.mUpdateVisitor->getTraversalMode();
            fixture.mStage.updateEye(*fixture.mUpdateVisitor);

            EXPECT_EQ(fixture.mEye->mReached, 1u) << "the eye was not updated";
            EXPECT_EQ(fixture.mWorld->mReached, 1u) << "the world was walked twice in one frame";
            EXPECT_EQ(fixture.mUpdateVisitor->getTraversalMode(), was) << "the visitor was handed back changed";
        }

        /// The eye's callback belongs to `MWRender::Camera` — attached in its constructor, removed
        /// in its destructor — so a camera carrying none is a frame outside that object's life.
        /// Asking for the eye then is a no-op rather than a crash.
        TEST(MWRenderStageTest, updatingTheEyeOfACameraWithNoCallbackDoesNothing)
        {
            Fixture fixture;
            fixture.mCamera->removeUpdateCallback(fixture.mEye);

            fixture.mStage.updateEye(*fixture.mUpdateVisitor);

            EXPECT_EQ(fixture.mEye->mReached, 0u);
            EXPECT_EQ(fixture.mWorld->mReached, 0u);
        }
    }
}
