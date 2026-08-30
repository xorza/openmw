#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include <osg/Vec3f>

#include "cellscene.hpp"
#include "framerequest.hpp"
#include "lighting.hpp"

namespace Resource
{
    class ImageManager;
}

namespace ESM
{
    struct Cell;
}

namespace Rtx
{
    struct ValidationOptions;
    class SceneDesc;
}

namespace RtxTool
{
    /// Everything the window needs that is not the world itself.
    struct ViewRequest
    {
        FrameRequest mFrame;

        std::string mTitle;

        /// The `views.cfg` id this was opened as, and that entry's note. Both empty where the window
        /// was opened by `--cell`. Carried so that flying somewhere better and pressing P prints a
        /// block that replaces the entry rather than one that has to be renamed by hand.
        std::string mView;
        std::string mNote;

        /// The cell, spelled the way `--cell` takes it: a pair of integers for an exterior, a name
        /// for an interior. Carried so the window can print a command line that comes back here.
        std::string mCell;
        std::filesystem::path mScreenshotDirectory;

        /// Write the albedo with no shading over it.
        bool mShowAlbedo = false;

        /// Filled in from the cell once it has been read, which is why both commands take their
        /// request by value.
        CellLighting mLighting;

        std::optional<osg::Vec3f> mOrigin;
        std::optional<osg::Vec3f> mTarget;

        /// Close after this many frames. Zero waits for someone to close the window; anything else
        /// is how the window path gets exercised by something that cannot click.
        std::uint32_t mFrames = 0;
    };

    /// Opens a window on the region around `centre` and flies around it until it is closed.
    ///
    /// **The window loads its own world rather than being handed one**, because it is the only
    /// caller whose camera goes somewhere: crossing into the next cell has to bring that cell's
    /// neighbours in and let the ones behind go, and nothing that took a finished scene could do
    /// that.
    int runWindow(World& world, const ESM::Cell& centre, const Rtx::ValidationOptions& validation, ViewRequest request);
}
