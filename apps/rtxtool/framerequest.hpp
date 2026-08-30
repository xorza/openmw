#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include <osg/Vec3f>

#include <components/rtx/reconstruction.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtx/upscale.hpp>

#include "posedactors.hpp"
#include "views.hpp"

namespace RtxTool
{
    struct StagingRequest;

    /// What a command's frames are traced with, and when and where the world stands for them.
    ///
    /// **One block and not four.** A shot, a window, a profiling run and an A/B differ in what they
    /// keep — a PNG, a swapchain, a table of times, a comparison — and in nothing about the frame
    /// itself. Held apart, each of the four named these fields itself, `dispatch` read the command
    /// line into them four times, and the two conversions below were written out by hand at each.
    struct FrameRequest
    {
        std::filesystem::path mShaderDirectory;

        /// The size the frame is presented at. What it is traced at follows from `mUpscale`.
        std::uint32_t mWidth = 1920;
        std::uint32_t mHeight = 1080;
        float mFieldOfView = 60.0f;

        /// Whether Ray Reconstruction stands between the trace and the picture, and how hard it
        /// works. It denoises for itself, so `mFilter` stops meaning anything once this is on.
        Rtx::Upscale mUpscale = Rtx::Upscale::Off;

        /// Which network it runs. Pinned rather than left to the library, whose own default has
        /// moved between SDK versions, so that two runs are comparable.
        Rtx::Preset mPreset = Rtx::Preset::D;

        /// How much of the lighting painted into each texture to divide back out, from zero to one.
        /// Zero shows the textures as they were drawn, with their lighting still in them.
        float mDelight = 1.0f;

        /// Whether the denoiser runs. Off is how a reference is made, and how the noise the filter
        /// is meant to remove can be looked at.
        bool mFilter = true;

        /// What to scale the frame by before the display curve, or nothing to measure it off the
        /// frame. A picture wants it measured; a reference wants it held still.
        std::optional<float> mExposure;

        /// When and in what weather, for the exterior that has a sky. A weather is named as the
        /// fallback settings spell it, and the hour is on a twenty-four hour clock.
        std::string mWeather = "Clear";

        /// The hour a place stands at where it fixes none of its own. `View::mHour` says which wins,
        /// and `describeStaging` is where that rule is applied.
        float mHour = sDefaultHour;

        /// Which day, counted from the one a new game begins on. Only the moons read it.
        int mDay = 0;

        ActorRequest mActors;

        /// The renderer these frames are traced by.
        ///
        /// @param window where the frames are shown, or null for one that only reads pixels back.
        Rtx::RendererOptions describeRenderer(
            const Rtx::ValidationOptions& validation, SDL_Window* window = nullptr) const;

        /// When and where the region stands, for a camera at `origin` looking at `target`.
        ///
        /// Both default to nothing, which is what a report wants: it is not taken from anywhere, and
        /// the commands that read one derive the camera from the region's own bounds.
        ///
        /// @param hour the place's own, which wins over `mHour`, or nothing where it fixes none.
        StagingRequest describeStaging(std::optional<float> hour = std::nullopt,
            const std::optional<osg::Vec3f>& origin = std::nullopt,
            const std::optional<osg::Vec3f>& target = std::nullopt) const;
    };
}
