#include <limits>
#include <memory>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include <osg/Callback>
#include <osg/Camera>
#include <osg/Group>
#include <osg/Node>
#include <osg/NodeVisitor>
#include <osg/ref_ptr>
#include <osgUtil/UpdateVisitor>

#include <apps/openmw/mwrender/rtx/rtxrun.hpp>
#include <components/resource/imagemanager.hpp>
#include <components/resource/objectcache.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/testing/util.hpp>
#include <components/vfs/manager.hpp>

#include "apps/openmw/mwrender/rtx/rtxrenderer.hpp"

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
        TEST(RtxRendererTest, updatingTheEyeRunsTheCamerasCallbackAndNothingBelowIt)
        {
            Fixture fixture;

            fixture.mSceneRoot->accept(*fixture.mUpdateVisitor);
            ASSERT_EQ(fixture.mWorld->mReached, 1u);
            ASSERT_EQ(fixture.mEye->mReached, 0u) << "the camera is not below the scene root";

            const osg::NodeVisitor::TraversalMode was = fixture.mUpdateVisitor->getTraversalMode();
            RtxRenderer::updateEye(*fixture.mCamera, *fixture.mUpdateVisitor);

            EXPECT_EQ(fixture.mEye->mReached, 1u) << "the eye was not updated";
            EXPECT_EQ(fixture.mWorld->mReached, 1u) << "the world was walked twice in one frame";
            EXPECT_EQ(fixture.mUpdateVisitor->getTraversalMode(), was) << "the visitor was handed back changed";
        }

        /// The eye's callback belongs to `MWRender::Camera` — attached in its constructor, removed
        /// in its destructor — so a camera carrying none is a frame outside that object's life.
        /// Asking for the eye then is a no-op rather than a crash.
        TEST(RtxRendererTest, updatingTheEyeOfACameraWithNoCallbackDoesNothing)
        {
            Fixture fixture;
            fixture.mCamera->removeUpdateCallback(fixture.mEye);

            RtxRenderer::updateEye(*fixture.mCamera, *fixture.mUpdateVisitor);

            EXPECT_EQ(fixture.mEye->mReached, 0u);
            EXPECT_EQ(fixture.mWorld->mReached, 0u);
        }

        /// **A stepped run keeps what it loaded, and a run on the wall keeps the setting.** The
        /// scene's cache and the images' are where a model and its textures are found again; both
        /// start at the setting's five seconds.
        TEST(RtxRendererTest, aSteppedRunNeverExpiresWhatItLoadedAndARunOnTheWallKeepsTheSetting)
        {
            constexpr double sSetting = 5.0;
            constexpr double sForever = std::numeric_limits<double>::infinity();
            const std::unique_ptr<VFS::Manager> vfs = TestingOpenMW::createTestVFS({});

            Resource::ResourceSystem stepped(vfs.get(), sSetting, nullptr);
            RtxRenderer::setResourceExpiry(stepped, sStepSeconds);
            EXPECT_EQ(stepped.getSceneManager()->getExpiryDelay(), sForever);
            EXPECT_EQ(stepped.getImageManager()->getExpiryDelay(), sForever);

            Resource::ResourceSystem walled(vfs.get(), sSetting, nullptr);
            RtxRenderer::setResourceExpiry(walled, std::nullopt);
            EXPECT_EQ(walled.getSceneManager()->getExpiryDelay(), sSetting);
            EXPECT_EQ(walled.getImageManager()->getExpiryDelay(), sSetting);
        }

        /// What the infinite delay promises of a cache: an item nothing references, last used at
        /// one second, is kept through an update a million seconds later — where the setting's
        /// five drop it at the same update.
        TEST(RtxRendererTest, anInfiniteDelayKeepsAnUnreferencedItemThatTheSettingDrops)
        {
            constexpr double sLater = 1.0e6;
            for (const double delay : { std::numeric_limits<double>::infinity(), 5.0 })
            {
                Resource::GenericObjectCache<std::string, osg::ref_ptr<osg::Object>> cache;
                cache.addEntryToObjectCache(std::string("model"), new osg::Group, 1.0);

                cache.update(sLater, delay);

                EXPECT_EQ(cache.getRefFromObjectCacheOrNone(std::string("model")).has_value(), delay > sLater)
                    << "at a delay of " << delay;
            }
        }
    }
}
