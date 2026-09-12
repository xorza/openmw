#include <gtest/gtest.h>

#include <osg/Camera>
#include <osg/Group>
#include <osg/NodeCallback>

#include <osgUtil/UpdateVisitor>

#include "apps/openmw/mwrender/renderer.hpp"

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

        /// A camera with the world under it, the way a renderer hangs the scene root.
        struct Fixture
        {
            osg::ref_ptr<osg::Camera> mCamera = new osg::Camera;
            osg::ref_ptr<osgUtil::UpdateVisitor> mUpdateVisitor = new osgUtil::UpdateVisitor;
            osg::ref_ptr<osg::Group> mSceneRoot = new osg::Group;

            osg::ref_ptr<CountingCallback> mEye = new CountingCallback;
            osg::ref_ptr<CountingCallback> mWorld = new CountingCallback;

            Fixture()
            {
                mCamera->addUpdateCallback(mEye);
                mSceneRoot->addUpdateCallback(mWorld);
                mCamera->addChild(mSceneRoot);
            }
        };

        /// **The eye is updated without the world being walked a second time.**
        ///
        /// A renderer that drives its own frame walks the scene from its own root — for the node
        /// path, which must not start at an `ABSOLUTE_RF` camera — and then wants the one callback
        /// the camera carries. Accepting on the camera to get it ran every animation controller,
        /// every `LightController` and `LightManager::update` twice in the same frame, at the same
        /// traversal number.
        TEST(MWRenderRendererTest, updatingTheEyeRunsTheCamerasCallbackAndNothingBelowIt)
        {
            Fixture fixture;

            fixture.mSceneRoot->accept(*fixture.mUpdateVisitor);
            ASSERT_EQ(fixture.mWorld->mReached, 1u);
            ASSERT_EQ(fixture.mEye->mReached, 0u) << "the camera is not below the scene root";

            const osg::NodeVisitor::TraversalMode was = fixture.mUpdateVisitor->getTraversalMode();
            updateEye(*fixture.mCamera, *fixture.mUpdateVisitor);

            EXPECT_EQ(fixture.mEye->mReached, 1u) << "the eye was not updated";
            EXPECT_EQ(fixture.mWorld->mReached, 1u) << "the world was walked twice in one frame";
            EXPECT_EQ(fixture.mUpdateVisitor->getTraversalMode(), was) << "the visitor was handed back changed";
        }

        /// The eye's callback belongs to `MWRender::Camera` — attached in its constructor, removed
        /// in its destructor — so a camera carrying none is a frame outside that object's life.
        /// Asking for the eye then is a no-op rather than a crash.
        TEST(MWRenderRendererTest, updatingTheEyeOfACameraWithNoCallbackDoesNothing)
        {
            Fixture fixture;
            fixture.mCamera->removeUpdateCallback(fixture.mEye);

            updateEye(*fixture.mCamera, *fixture.mUpdateVisitor);

            EXPECT_EQ(fixture.mEye->mReached, 0u);
            EXPECT_EQ(fixture.mWorld->mReached, 0u);
        }
    }
}
