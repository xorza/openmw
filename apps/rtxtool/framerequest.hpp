#pragma once

#include <cstdint>

#include <components/rtx/renderprofile.hpp>
#include <components/rtx/upscale.hpp>

#include "views.hpp"

namespace RtxTool
{
    /// What a frame is upscaled by when nobody names a mode.
    ///
    /// **It follows the build**, because the two are one decision: `-DOPENMW_RTX_DLSS=OFF` is a
    /// deliberate opt-out, and a tool that then refused every default invocation would be telling
    /// its user to turn on the thing they had just turned off.
    ///
    /// Quality rather than performance, so a plain run is the renderer with everything switched on
    /// and not one that quietly quartered the pixels it traced.
#ifdef OPENMW_RTX_DLSS
    inline constexpr Rtx::Upscale sUpscaleByDefault = Rtx::Upscale::Quality;
#else
    inline constexpr Rtx::Upscale sUpscaleByDefault = Rtx::Upscale::Off;
#endif

    /// What a command's frames are traced with.
    ///
    /// **One block and not four.** A shot, a window, a profiling run and an A/B differ in what they
    /// keep — a PNG, a swapchain, a table of times, a comparison — and in nothing about the frame
    /// itself. Held apart, each of the four named these fields itself and `dispatch` read the
    /// command line into them four times.
    ///
    /// **Where a run stands is not here.** The hour and the sky belong to the place, because a view
    /// may fix either and the command line may overrule it — `stopFor` is where the two meet.
    struct FrameRequest
    {
        /// The size the frame is presented at. What it is traced at follows from `mProfile.mUpscaling`.
        std::uint32_t mWidth = 1920;
        std::uint32_t mHeight = 1080;
        float mFieldOfView = 60.0f;

        /// How far out from the eye the world is built, in cells.
        ///
        /// **How much world exists, which is a property of the structure rays are cast against and
        /// not of the camera.** The air is tuned to it as well as the ground, so a ring of ground
        /// four cells out and a fog measured over thirty thousand units are one number.
        float mDistantCells = 4.0f;

        /// Whether what the content files stand on the distant ground is paged in with it.
        ///
        /// **The game's own `object paging`, set rather than answered here.** What a run measures
        /// is the paging a player gets, so the A/B that says what the buildings and the trees cost
        /// is that setting turned off and not a second way of building the world. The ground itself
        /// carries no flag: `Renderer::wantsPagedTerrain` says why a tracing renderer always pages
        /// it.
        bool mDistantStatics = true;

        /// Which day, counted from the one a new game begins on. Only the moons read it.
        int mDay = 0;

        /// What the trace itself is configured by, handed to the renderer through `RendererSpec`.
        ///
        /// **Held whole rather than spelled out again.** Everything above is the engine's — a
        /// window, a camera, how much world to build — and everything the trace decides is one type
        /// that the game reads out of `[RTX]` and this fills from the command line.
        Rtx::RenderProfile mProfile{ .mUpscaling = { .mMode = sUpscaleByDefault } };
    };
}
