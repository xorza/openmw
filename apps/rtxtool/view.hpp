#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include <osg/Vec3f>

#include <components/rtx/reconstruction.hpp>
#include <components/rtx/upscale.hpp>

#include "cellscene.hpp"
#include "lighting.hpp"
#include "posedactors.hpp"

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
        std::string mTitle;

        /// The `views.cfg` id this was opened as, and that entry's note. Both empty where the window
        /// was opened by `--cell`. Carried so that flying somewhere better and pressing P prints a
        /// block that replaces the entry rather than one that has to be renamed by hand.
        std::string mView;
        std::string mNote;

        /// The cell, spelled the way `--cell` takes it: a pair of integers for an exterior, a name
        /// for an interior. Carried so the window can print a command line that comes back here.
        std::string mCell;
        std::filesystem::path mShaderDirectory;
        std::filesystem::path mScreenshotDirectory;

        std::uint32_t mWidth = 1920;
        std::uint32_t mHeight = 1080;
        float mFieldOfView = 60.0f;

        /// How much of the lighting painted into each texture to divide back out, from zero to one.
        /// Zero shows the textures as they were drawn, with their lighting still in them.
        float mDelight = 1.0f;

        /// Whether the denoiser runs. Off is what shows the noise it is there to remove.
        bool mFilter = true;

        /// Write the albedo with no shading over it.
        bool mShowAlbedo = false;

        /// What to scale the frame by before the display curve, or nothing to measure it off the
        /// frame. A picture wants it measured; a reference wants it held still.
        std::optional<float> mExposure;

        /// Whether Ray Reconstruction stands between the trace and the window, and how hard it
        /// works. It denoises for itself, so `mFilter` stops meaning anything once this is on.
        Rtx::Upscale mUpscale = Rtx::Upscale::Off;

        /// Which network it runs. Pinned rather than left to the library, whose own default has
        /// moved between SDK versions, so that two runs are comparable.
        Rtx::Preset mPreset = Rtx::Preset::D;

        /// Filled in from the cell once it has been read, which is why both commands take their
        /// request by value.
        CellLighting mLighting;

        /// When and in what weather, for the exterior that has a sky. A weather is named as the
        /// fallback settings spell it, and the hour is on a twenty-four hour clock.
        std::string mWeather = "Clear";
        float mHour = 12.0f;

        /// Which day, counted from the one a new game begins on. Only the moons read it.
        int mDay = 0;

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
    int runWindow(World& world, const ESM::Cell& centre, const Rtx::ValidationOptions& validation, ViewRequest request,
        const ActorRequest& actors);

}
