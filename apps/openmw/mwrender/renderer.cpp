#include "renderer.hpp"

#include <cassert>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>

#include <SDL_hints.h>
#include <SDL_video.h>

#include <osg/Camera>
#include <osg/FrameStamp>
#include <osg/Group>
#include <osg/Image>
#include <osg/Stats>

#include <osgDB/ReaderWriter>
#include <osgDB/Registry>

#include <osgGA/EventQueue>

#include <osgUtil/UpdateVisitor>

#include <components/debug/debuglog.hpp>
#include <components/l10n/manager.hpp>
#include <components/sceneutil/screencapture.hpp>
#include <components/sceneutil/workqueue.hpp>
#include <components/sdlutil/imagetosurface.hpp>
#include <components/settings/values.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwgui/messagebox.hpp"

#include "gl/glrenderer.hpp"

#ifdef OPENMW_RTX
#include "rtx/rtxrenderer.hpp"
#endif

namespace MWRender
{
    namespace
    {
        struct ScreenCaptureMessageBox
        {
            void operator()(std::string filePath) const
            {
                if (filePath.empty())
                {
                    MWBase::Environment::get().getWindowManager()->scheduleMessageBox(
                        "#{OMWEngine:ScreenshotFailed}", MWGui::ShowInDialogueMode_Never);

                    return;
                }

                auto l10n = MWBase::Environment::get().getL10nManager()->getContext("OMWEngine");
                std::string message = l10n->formatMessage("ScreenshotMade", { "file" }, { L10n::toUnicode(filePath) });

                MWBase::Environment::get().getWindowManager()->scheduleMessageBox(
                    std::move(message), MWGui::ShowInDialogueMode_Never);
            }
        };

        struct IgnoreString
        {
            void operator()(std::string) const {}
        };
    }

    Renderer::~Renderer() = default;

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

    void setWindowIcon(SDL_Window& window, const std::filesystem::path& resourceDir)
    {
        const std::filesystem::path windowIcon = resourceDir / "openmw.png";

        std::ifstream stream(windowIcon, std::ios_base::in | std::ios_base::binary);
        if (stream.fail())
        {
            Log(Debug::Error) << "Error: Failed to open " << windowIcon;
            return;
        }

        osgDB::ReaderWriter* reader = osgDB::Registry::instance()->getReaderWriterForExtension("png");
        if (reader == nullptr)
        {
            Log(Debug::Error) << "Error: Failed to read window icon, no png readerwriter found";
            return;
        }

        osgDB::ReaderWriter::ReadResult result = reader->readImage(stream);
        if (!result.success())
        {
            Log(Debug::Error) << "Error: Failed to read " << windowIcon << ": " << result.message() << " code "
                              << result.status();
            return;
        }

        const osg::ref_ptr<osg::Image> image = result.getImage();
        const auto surface = SDLUtil::imageToSurface(image, true);
        SDL_SetWindowIcon(&window, surface.get());
    }

    osg::ref_ptr<SceneUtil::AsyncScreenCaptureOperation> makeScreenshotWriter(
        SceneUtil::WorkQueue& queue, const std::filesystem::path& screenshotPath)
    {
        return new SceneUtil::AsyncScreenCaptureOperation(&queue,
            new SceneUtil::WriteScreenshotToFileOperation(screenshotPath, Settings::general().mScreenshotFormat,
                Settings::general().mNotifyOnSavedScreenshot
                    ? std::function<void(std::string)>(ScreenCaptureMessageBox{})
                    : std::function<void(std::string)>(IgnoreString{})));
    }
}
