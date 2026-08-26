#ifndef GAME_RENDER_WINDOWSETUP_H
#define GAME_RENDER_WINDOWSETUP_H

#include <filesystem>

#include <SDL_stdinc.h>

struct SDL_Window;

namespace MWRender
{
    /// Where a window goes and what it is, as the video settings ask for it.
    struct WindowPlacement
    {
        int mX = 0;
        int mY = 0;
        int mWidth = 0;
        int mHeight = 0;
        Uint32 mFlags = 0;
    };

    /// What every renderer asks SDL for, and the hints it has to have set before asking.
    ///
    /// **Everything here is the video settings and none of it is a graphics API.** Screen, size,
    /// window mode, border, what happens on focus loss — two renderers wanted the same twenty lines
    /// and the only difference between them is one flag naming what will be drawn into the surface.
    ///
    /// **The hints are set here because this is the last moment they can be.** SDL reads them inside
    /// `SDL_CreateWindow`, so a caller that set them afterwards would be setting them for the next
    /// window.
    ///
    /// @param surfaceFlag what the window is for: `SDL_WINDOW_OPENGL` for the rasterizer, whatever
    ///        `Rtx::surfaceWindowFlag` answers for the ray tracer.
    WindowPlacement describeWindow(Uint32 surfaceFlag);

    /// Reads `openmw.png` from beside the resources and gives it to the window.
    ///
    /// Logs and carries on wherever it cannot: a window with no icon is still a window, and this
    /// runs before there is anything on screen to report a failure with.
    void setWindowIcon(SDL_Window& window, const std::filesystem::path& resourceDir);
}

#endif
