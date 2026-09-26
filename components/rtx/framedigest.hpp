#pragma once

#include <array>
#include <cstdint>

#include "shaders/digest.h"

namespace Rtx
{
    /// A hundred and twenty-eight bits that name one image's every bit, as `shaders/digest.h` folds
    /// them.
    using DigestWords = std::array<std::uint64_t, 2>;

    /// What a frame computed before anything past it had a say: every channel the trace wrote and
    /// the composite that folded them, digested on the device, and the numbers the frame handed the
    /// reconstruction beside those images. What a comparison of two runs compares. The picture is
    /// still hashed beside this, but where a network reconstructed it the picture is the network's:
    /// it keeps a history, and the smallest difference in what it was handed on one frame stays in
    /// its picture for the rest of the run — so the picture there is something to look at and never
    /// a verdict, and this is the verdict.
    struct FrameDigest
    {
        /// The channels at their binding, and the composite at `Shaders::DIGEST_COMPOSITE`.
        std::array<DigestWords, Shaders::DIGEST_IMAGES> mImages{};

        /// Where inside the pixel this frame sampled, as the trace applied it — what the
        /// reconstruction is told, negated, to cancel.
        float mJitterX = 0.0f;
        float mJitterY = 0.0f;

        /// How long the frame stood for, which the reconstruction tunes itself by.
        float mFrameDeltaMs = 0.0f;

        /// Whether the frame told the upscaler its history was worthless: never, where nothing
        /// upscaled it.
        std::uint32_t mReset = 0;
    };
}
