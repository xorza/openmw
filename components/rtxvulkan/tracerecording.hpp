#pragma once

#include <cstdint>

#include <components/rtx/shaders/visibility.h>

#include "visibilitypass.hpp"

namespace Rtx
{
    class Buffer;
    class CompositePass;
    class GpuTimer;
    class Graveyard;
    class Image;
    class SceneBuffers;
    class SpriteBinPass;
    class SpriteShadePass;

    /// What one camera's trace records against, and what makes this trace different from the other.
    ///
    /// **The recording's own context and not the renderer's**, for the reason `Placing` gives. Two
    /// cameras record one chain — a frame, and a picture inside the interface — and what they share
    /// is this list rather than an object. Nothing here is held: every field is per-call data, so
    /// naming them costs `TraceChain` no member and no lifetime.
    struct TraceRecording
    {
        /// The passes every trace runs, whichever camera it is for. Borrowed from the renderer,
        /// which keeps one of each: what differs between two traces is the scene and the camera.
        const VisibilityPass* mVisibility = nullptr;
        const CompositePass* mComposite = nullptr;
        const SpriteBinPass* mSpriteBin = nullptr;
        const SpriteShadePass* mSpriteShade = nullptr;

        /// What the rays meet, and where the sea and the sprites the trace reads were left.
        VisibilityInputs mInputs;

        /// Where the sprite bin writes, which is `mInputs.mSlot`'s copy of the tables — the one this
        /// trace is about to read. The caller has waited for whatever was reading it.
        SceneBuffers* mBuffers = nullptr;
        Graveyard* mGraveyard = nullptr;

        /// The camera the caller asked for. **What the sprite bin tiles against**, because a bin is
        /// a screen-space tile and the jitter below is where inside a pixel this frame sampled:
        /// binning against that would move every tile by a fraction of a pixel a frame, for nothing.
        Shaders::VisibilityConstants mAsked;

        /// The same camera as this trace will sample it — the jitter, the previous basis, the
        /// medium and the layer decision are already in it. What the composite covers is its extent.
        Shaders::VisibilityConstants mSampled;

        /// What the trace sums its census into. The frame's, always: a picture drawn between two
        /// frames is not counted, and the buffer is bound because the shader writes it regardless.
        const Buffer* mCounts = nullptr;

        /// What the display curve will write into, discarded beside the chain's colour because both
        /// are rewritten whole.
        const Image* mTarget = nullptr;

        /// The running total a reference is built out of, and null where nothing is averaging.
        const Image* mSum = nullptr;
        std::uint32_t mAccumulate = 0;

        /// Whether the volume and the denoisers have a past to reproject from.
        bool mAirLost = true;
        bool mHistoryLost = true;

        /// Whether the wavelet runs. False for a frame an upscaler will denoise itself —
        /// `Reconstruction` is what resolves that, and never answers with both.
        bool mFilter = true;

        /// Null where the run is not being timed, which a picture is not.
        GpuTimer* mTimer = nullptr;
    };
}
