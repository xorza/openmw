#include "stage.hpp"

#include <cassert>

#include <osg/Camera>
#include <osg/FrameStamp>
#include <osg/Group>
#include <osg/Stats>

#include <osgGA/EventQueue>

#include <osgUtil/UpdateVisitor>

namespace MWRender
{
    Stage::Stage() = default;

    Stage::~Stage() = default;

    void Stage::adopt(osg::Camera& camera, osg::FrameStamp& frameStamp, osgGA::EventQueue& events, osg::Stats& stats)
    {
        mCamera = &camera;
        mFrameStamp = &frameStamp;
        mEvents = &events;
        mStats = &stats;
    }

    osg::Camera& Stage::getCamera() const
    {
        assert(mCamera != nullptr && "the camera is the renderer's to adopt, and nothing has yet");
        return *mCamera;
    }

    osg::FrameStamp& Stage::getFrameStamp() const
    {
        assert(mFrameStamp != nullptr && "the frame stamp is the renderer's to adopt, and nothing has yet");
        return *mFrameStamp;
    }

    osgGA::EventQueue& Stage::getEvents() const
    {
        assert(mEvents != nullptr && "the event queue is the renderer's to adopt, and nothing has yet");
        return *mEvents;
    }

    osg::Stats& Stage::getStats() const
    {
        assert(mStats != nullptr && "the stats are the renderer's to adopt, and nothing has yet");
        return *mStats;
    }

    osg::Group& Stage::getSceneRoot() const
    {
        assert(mSceneRoot != nullptr && "nothing is topmost until a renderer says so");
        return *mSceneRoot;
    }

    void Stage::setSceneRoot(osg::Group& root)
    {
        mSceneRoot = &root;

        // **And under the camera, because that is what a visitor is accepted on.** Nothing culls in
        // the ray tracing path and its camera draws nothing — the trace has its own — but
        // `RenderingManager::castCameraToViewportRay` still accepts an intersection visitor on
        // `getCamera()`, and that is how the game asks what the player is looking at. A camera with
        // no children answers "nothing": no door, no container, no person, and a world that cannot
        // be touched at all.
        //
        // **Here rather than in either renderer**, because this is the pair that must not disagree
        // and a renderer that had to remember would eventually be one that did not. Idempotent
        // because the rasterizer parents the same root again through `osgViewer::Viewer`.
        assert(mCamera != nullptr && "the camera is the renderer's to adopt, and nothing has yet");

        if (!mCamera->containsNode(&root))
            mCamera->addChild(&root);
    }

    void Stage::updateEye(osgUtil::UpdateVisitor& visitor) const
    {
        osg::Camera& camera = getCamera();
        if (camera.getUpdateCallback() == nullptr)
            return;

        // The mode is the visitor's own property, and a caller that lent one gets it back as it was.
        const osg::NodeVisitor::TraversalMode was = visitor.getTraversalMode();
        visitor.setTraversalMode(osg::NodeVisitor::TRAVERSE_NONE);
        camera.accept(visitor);
        visitor.setTraversalMode(was);
    }
}
