#pragma once

#include <cstdint>

#include <components/rtx/shaders/visibility.h>

#include "frameslots.hpp"
#include "visibilitypass.hpp"

namespace Rtx
{
    class Buffer;
    class GpuTimer;
    class Image;

    /// What one camera's trace records against, and what makes this trace different from the
    /// other: a frame and a picture inside the interface record one chain, and what they share is
    /// this list rather than an object. Nothing here is held.
    struct TraceRecording
    {
        /// What the rays meet, where the sea and the sprites the trace reads were left, and what
        /// every launch binds beside them: the census and the chain's slot. The chain's own images
        /// are the chain's to name (`TraceChain::record`).
        VisibilityInputs mInputs;

        /// The camera the caller asked for. What the sprite bin tiles against, because a bin is
        /// a screen-space tile and the jitter below is where inside a pixel this frame sampled:
        /// binning against that would move every tile by a fraction of a pixel a frame, for nothing.
        Shaders::VisibilityConstants mAsked;

        /// The same camera as this trace will sample it — the jitter, the previous basis, the
        /// medium and the layer decision are already in it. What the composite covers is its extent.
        Shaders::VisibilityConstants mSampled;

        /// What the display curve will write into, discarded beside the chain's colour because both
        /// are rewritten whole.
        const Image* mTarget = nullptr;

        /// How many frames the chain's running total holds, this one included, or nought where
        /// nothing is averaging (`FrameOptions::mAccumulate`).
        std::uint32_t mAccumulate = 0;

        /// Whether the camera has no past to reproject from: a picture never has one, and a frame
        /// after a jump no motion vector can describe has lost it. What `TraceChain::resetHistory`
        /// said is the chain's own to add.
        bool mPastLost = true;

        /// Whether the wavelet runs. False for a frame an upscaler will denoise itself —
        /// `Reconstruction` is what resolves that, and never answers with both.
        bool mFilter = true;

        /// Null where the run is not being timed, which a picture is not.
        GpuTimer* mTimer = nullptr;
    };

    /// What one trace hands the display: its inputs as the chain completed them — its own channels
    /// and air named — the composite's output, and the sprite tile list the trace read, which the
    /// curve tests for where the puffs' composite drew nothing.
    struct TraceResult
    {
        VisibilityInputs mInputs;
        const Image& mColour;
        VkDeviceAddress mSpriteTileList = 0;
    };
}
