#include "renderer.hpp"

#include <cassert>
#include <stdexcept>
#include <string>

#include <osg/Camera>
#include <osg/FrameStamp>
#include <osg/Group>
#include <osg/Stats>

#include <osgGA/EventQueue>

#include <osgUtil/UpdateVisitor>

#include <components/surface/material.hpp>

#include "gl/glrenderer.hpp"

#ifdef OPENMW_RTX
#include "rtx/rtxrenderer.hpp"
#endif

namespace MWRender
{
    Renderer::~Renderer() = default;

    int Renderer::getMaxTextureUnits() const
    {
        return Surface::sAssumedTextureUnits;
    }

    void Renderer::adopt(osg::Camera& camera, osg::FrameStamp& frameStamp, osgGA::EventQueue* events, osg::Stats& stats)
    {
        mCamera = &camera;
        mFrameStamp = &frameStamp;
        mEvents = events;
        mStats = &stats;
    }

    osg::Camera& Renderer::getCamera() const
    {
        assert(mCamera != nullptr && "the camera is the renderer's to adopt, and nothing has yet");
        return *mCamera;
    }

    osg::FrameStamp& Renderer::getFrameStamp() const
    {
        assert(mFrameStamp != nullptr && "the frame stamp is the renderer's to adopt, and nothing has yet");
        return *mFrameStamp;
    }

    osg::Stats& Renderer::getStats() const
    {
        assert(mStats != nullptr && "the stats are the renderer's to adopt, and nothing has yet");
        return *mStats;
    }

    osg::Group& Renderer::getSceneRoot() const
    {
        assert(mSceneRoot != nullptr && "nothing is topmost until a renderer says so");
        return *mSceneRoot;
    }

    void Renderer::setSceneRoot(osg::Group& root)
    {
        mSceneRoot = &root;

        osg::Camera& camera = getCamera();
        if (!camera.containsNode(&root))
            camera.addChild(&root);

        adoptSceneRoot(root);
    }

    void updateEye(osg::Camera& camera, osgUtil::UpdateVisitor& visitor)
    {
        if (camera.getUpdateCallback() == nullptr)
            return;

        const osg::NodeVisitor::TraversalMode was = visitor.getTraversalMode();
        visitor.setTraversalMode(osg::NodeVisitor::TRAVERSE_NONE);
        camera.accept(visitor);
        visitor.setTraversalMode(was);
    }

    std::unique_ptr<Renderer> createRenderer(std::string_view name, const RendererSpec& spec)
    {
        if (name == "opengl")
            return std::make_unique<GlRenderer>(spec);

#ifdef OPENMW_RTX
        if (name == "raytrace")
            return std::make_unique<RtxRenderer>(spec);
#endif

        // **Named rather than fallen back from.** A renderer that quietly became a different one
        // answers "why does it look like that" with silence, and a build without the one asked for
        // is a configuration mistake rather than a runtime condition.
        throw std::runtime_error("this build has no renderer named \"" + std::string(name) + '"');
    }
}
