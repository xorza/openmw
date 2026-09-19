#include "rtxwindow.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

#include <SDL_error.h>
#include <SDL_stdinc.h>
#include <osg/Camera>

#include <components/rtx/renderer.hpp>

#include "../renderer.hpp"

namespace MWRender
{
    namespace
    {
        /// How long the window must report one size before the renderer is rebuilt for it.
        ///
        /// **Because rebuilding costs about as long as this waits.** A new extent releases every
        /// target, allocates them again and uploads Ray Reconstruction's weights for the pair of
        /// resolutions it is now between — about a tenth of a second. A window dragged across a
        /// screen passes through hundreds of extents, and following each of them would draw the
        /// drag at ten frames a second.
        ///
        /// So a gesture is followed once it stops. Until then the surface keeps the extent it has,
        /// and what the compositor shows is that picture scaled — which is what a window being
        /// dragged shows anyway.
        ///
        /// **Six frames at sixty.** Long enough that a drag settles into one rebuild, short enough
        /// that letting go of a window edge and seeing the picture follow reads as immediate.
        constexpr double sSettleSeconds = 0.1;
    }

    RtxWindow::RtxWindow(const bool hidden)
    {
        // **The backend's own flag, and no `SDL_GL_SetAttribute` anywhere near it.** No GL context is
        // ever made, which is the point of the whole path.
        applyWindowHints();
        const WindowPlacement placement = describeWindow(SDL_WINDOW_VULKAN);

        // **Hidden and not absent.** A surface still needs a window, and a swapchain built on one
        // nobody is looking at costs a present per frame and nothing else — so a headless run is
        // the same renderer rather than a second path through it. `SDL_WINDOW_HIDDEN` also keeps
        // the compositor from raising a window over whatever the person running it is doing.
        const Uint32 flags = hidden ? (placement.mFlags | SDL_WINDOW_HIDDEN) : placement.mFlags;

        mWindow.reset(
            SDL_CreateWindow("OpenMW", placement.mX, placement.mY, placement.mWidth, placement.mHeight, flags));
        if (mWindow == nullptr)
            throw std::runtime_error(std::string("failed to create SDL window: ") + SDL_GetError());

        // Read once here, and `mAskedSince` left at nought, so the first `fit` acts on a size that
        // counts as settled rather than waiting the settle out with no viewport set.
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        readSize(width, height);
        mAskedWidth = width;
        mAskedHeight = height;
    }

    void RtxWindow::readSize(std::uint32_t& width, std::uint32_t& height) const
    {
        int wide = 0;
        int high = 0;
        SDL_GetWindowSizeInPixels(mWindow.get(), &wide, &high);
        width = static_cast<std::uint32_t>(std::max(wide, 1));
        height = static_cast<std::uint32_t>(std::max(high, 1));
    }

    void RtxWindow::fit(Rtx::Renderer& renderer, osg::Camera& camera)
    {
        const osg::Timer_t now = osg::Timer::instance()->tick();

        std::uint32_t width = 0;
        std::uint32_t height = 0;
        readSize(width, height);
        if (width != mAskedWidth || height != mAskedHeight)
        {
            mAskedWidth = width;
            mAskedHeight = height;
            mAskedSince = now;
        }

        if (osg::Timer::instance()->delta_s(mAskedSince, now) < sSettleSeconds)
            return;

        renderer.resize(mAskedWidth, mAskedHeight);

        // **The renderer's own extent and not SDL's.** A windowed backend sizes itself to the
        // surface, and on a scaled or tiling compositor that is not what the window was asked for.
        // Everything above reads the viewport, so it has to be told what was actually built.
        const Rtx::FrameExtents extents = renderer.getExtents();
        camera.setViewport(0, 0, static_cast<int>(extents.mOutputWidth), static_cast<int>(extents.mOutputHeight));
    }

    void RtxWindow::setTitle(const char* title)
    {
        if ((SDL_GetWindowFlags(mWindow.get()) & SDL_WINDOW_HIDDEN) != 0)
            return;

        SDL_SetWindowTitle(mWindow.get(), title);
    }
}
