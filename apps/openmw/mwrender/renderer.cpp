#include "renderer.hpp"

#include <cassert>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include <SDL_hints.h>
#include <SDL_video.h>

#include <osg/Camera>
#include <osg/FrameStamp>
#include <osg/Group>
#include <osg/Stats>

#include <components/misc/frameratelimiter.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/sceneutil/screencapture.hpp>
#include <components/settings/values.hpp>
#include <components/shader/automaps.hpp>

#include "glrenderer.hpp"

#ifdef OPENMW_RTX
#include "rtx/rtxrenderer.hpp"
#endif

namespace MWRender
{
    Renderer::Renderer() = default;
    Renderer::~Renderer() = default;

    void Renderer::prepareResources(Resource::ResourceSystem& resources)
    {
        mResources = &resources;

        // What the content's companion maps are called and whether to look for them, which both
        // renderers read the same way: the files are the content's, whoever draws them.
        const Settings::ShadersCategory& shaders = Settings::shaders();
        resources.getSceneManager()->setAutoMaps(Shader::AutoMapRules{
            .mNormalMaps = shaders.mAutoUseObjectNormalMaps,
            .mNormalMapPattern = shaders.mNormalMapPattern,
            .mNormalHeightMapPattern = shaders.mNormalHeightMapPattern,
            .mSpecularMaps = shaders.mAutoUseObjectSpecularMaps,
            .mSpecularMapPattern = shaders.mSpecularMapPattern,
        });

        configureResources(resources);
    }

    Resource::ResourceSystem& Renderer::getResources() const
    {
        assert(mResources != nullptr && "the resource system is Engine's to hand over, and it has not yet");
        return *mResources;
    }

    const Misc::FrameClock& Renderer::getFrameClock() const
    {
        assert(mClock != nullptr && "a frame before the host's clock was handed over");
        return *mClock;
    }

    void Renderer::setFrameRateLimit(const float limit)
    {
        mFrameRateLimit = limit;
        mLimiter = Misc::makeFrameRateLimiter(limit);
        applyFrameRateLimit();
    }

    std::chrono::steady_clock::duration Renderer::awaitFrame()
    {
        const std::chrono::steady_clock::time_point began = std::chrono::steady_clock::now();
        const bool held = holdFrame();
        if (!held)
            mLimiter.limit();

        const std::chrono::steady_clock::time_point opened = std::chrono::steady_clock::now();
        mLastHold = opened - began;

        const std::optional<std::chrono::steady_clock::time_point> before = std::exchange(mOpened, opened);
        if (!held)
            return mLimiter.getLastFrameDuration();

        // The limiter measures its next frame from here, as if it had held this one.
        mLimiter = Misc::makeFrameRateLimiter(mFrameRateLimit, opened);
        return before.has_value() ? opened - *before : std::chrono::steady_clock::duration::zero();
    }

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
        applyViewMask();
    }

    void Renderer::showWorld(const bool shown)
    {
        // Asked every frame by the window manager, and answered on the change alone.
        if (mWorldShown == shown)
            return;

        mWorldShown = shown;
        applyWorldShown();
    }

    bool Renderer::toggleRenderMode(const RenderMode mode)
    {
        if (mode != Render_Scene)
            return toggleOwnRenderMode(mode);

        mWorldToggled = !mWorldToggled;
        applyWorldShown();
        return mWorldToggled;
    }

    void Renderer::setTraversalRoot(osg::Group& root)
    {
        mTraversalRoot = &root;
        adoptTraversalRoot(root);
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

        return placement;
    }

    void applyWindowHints()
    {
        // Allows for Windows snapping features to properly work in borderless window
        SDL_SetHint("SDL_BORDERLESS_WINDOWED_STYLE", "1");
        SDL_SetHint("SDL_BORDERLESS_RESIZABLE_STYLE", "1");
        SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, Settings::video().mMinimizeOnFocusLoss ? "1" : "0");
    }
}
