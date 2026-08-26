#include "windowsetup.hpp"

#include <fstream>

#include <SDL_hints.h>
#include <SDL_video.h>

#include <osg/Image>
#include <osg/ref_ptr>

#include <osgDB/ReaderWriter>
#include <osgDB/Registry>

#include <components/debug/debuglog.hpp>
#include <components/sdlutil/imagetosurface.hpp>
#include <components/settings/values.hpp>

namespace MWRender
{
    WindowPlacement describeWindow(Uint32 surfaceFlag)
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
}
