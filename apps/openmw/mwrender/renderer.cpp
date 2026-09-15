#include "renderer.hpp"

#include <cassert>
#include <stdexcept>
#include <string>

#include <SDL_hints.h>
#include <SDL_video.h>

#include <osg/Camera>
#include <osg/FrameStamp>
#include <osg/Group>
#include <osg/Stats>

#include <components/sceneutil/screencapture.hpp>
#include <components/settings/values.hpp>

#include "glrenderer.hpp"

#ifdef OPENMW_RTX
#include "rtx/rtxrenderer.hpp"
#endif

namespace MWRender
{
    Renderer::~Renderer() = default;

    void Renderer::setScreenshotWriter(SceneUtil::AsyncScreenCaptureOperation& writer)
    {
        mScreenshotWriter = &writer;
    }

    SceneUtil::AsyncScreenCaptureOperation& Renderer::getScreenshotWriter() const
    {
        assert(mScreenshotWriter != nullptr && "the screenshot writer is Engine's to hand over, and it has not yet");
        return *mScreenshotWriter;
    }

    void Renderer::adopt(osg::Camera& camera, osg::FrameStamp& frameStamp, osg::Stats& stats)
    {
        mCamera = &camera;
        mFrameStamp = &frameStamp;
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

    osg::Group& Renderer::getTraversalRoot() const
    {
        assert(mTraversalRoot != nullptr && "nothing is topmost until a renderer says so");
        return *mTraversalRoot;
    }

    void Renderer::renderGuiFrame()
    {
        eventTraversal();
        updateTraversal();
        renderGui();
        advance(getFrameStamp().getSimulationTime());
    }

    void Renderer::renderLoadingFrame(const double targetFrameRate)
    {
        applyLoadingBudget(targetFrameRate);
        renderGuiFrame();
    }

    void Renderer::setViewMask(const unsigned int mask)
    {
        mViewMask = mask;
        applyViewMask(mask);
    }

    void Renderer::setTraversalRoot(osg::Group& root)
    {
        mTraversalRoot = &root;
        adoptTraversalRoot(root);
    }

    std::unique_ptr<Renderer> createRenderer(std::string_view name, const RendererSpec& spec)
    {
        if (name == "opengl")
        {
            // A run states what a measured frame is, and the rasterizer measures nothing: the
            // harness that installed one has asked for the other renderer and not said so.
            if (spec.mRtx != nullptr)
                throw std::runtime_error("a ray tracing run was installed, and the renderer asked for is \"opengl\"");

            return std::make_unique<GlRenderer>(spec);
        }

#ifdef OPENMW_RTX
        if (name == "raytrace")
            return std::make_unique<RtxRenderer>(spec);
#endif

        // **Named rather than fallen back from.** A renderer that quietly became a different one
        // answers "why does it look like that" with silence, and a build without the one asked for
        // is a configuration mistake rather than a runtime condition.
        throw std::runtime_error("this build has no renderer named \"" + std::string(name) + '"');
    }

    WindowPlacement describeWindow(const std::uint32_t surfaceFlag)
    {
        const Settings::WindowMode windowMode = Settings::video().mWindowMode;
        const int screen = Settings::video().mScreen;

        WindowPlacement placement;
        placement.mWidth = Settings::video().mResolutionX;
        placement.mHeight = Settings::video().mResolutionY;

        // A fullscreen window is placed by the display it names rather than centred on it.
        const bool fullscreen
            = windowMode == Settings::WindowMode::Fullscreen || windowMode == Settings::WindowMode::WindowedFullscreen;
        placement.mX = fullscreen ? SDL_WINDOWPOS_UNDEFINED_DISPLAY(screen) : SDL_WINDOWPOS_CENTERED_DISPLAY(screen);
        placement.mY = placement.mX;

        placement.mFlags = surfaceFlag | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
        if (windowMode == Settings::WindowMode::Fullscreen)
            placement.mFlags |= SDL_WINDOW_FULLSCREEN;
        else if (windowMode == Settings::WindowMode::WindowedFullscreen)
            placement.mFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
        if (!Settings::video().mWindowBorder)
            placement.mFlags |= SDL_WINDOW_BORDERLESS;

        // Allows for Windows snapping features to properly work in borderless window
        SDL_SetHint("SDL_BORDERLESS_WINDOWED_STYLE", "1");
        SDL_SetHint("SDL_BORDERLESS_RESIZABLE_STYLE", "1");
        SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, Settings::video().mMinimizeOnFocusLoss ? "1" : "0");

        return placement;
    }
}
