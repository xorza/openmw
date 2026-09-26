#pragma once

#include <cstdint>

namespace Rtx
{
    /// How far a renderer's kernels have come: how many are made, of how many. What
    /// `Renderer::awaitKernels` answers, for a loading screen to show while it waits.
    struct KernelProgress
    {
        std::uint32_t mMade = 0;
        std::uint32_t mCount = 0;

        /// Whether the compile is over, which is the one moment `mMade` reaches `mCount`: a kernel
        /// made is counted as it lands, and the last is not counted until the compile has returned.
        bool isDone() const { return mMade == mCount; }
    };
}
