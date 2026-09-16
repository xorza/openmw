#pragma once

#include <filesystem>

#include <vulkan/vulkan_core.h>

#include "buffer.hpp"
#include "computepipeline.hpp"

namespace Rtx
{
    class Device;

    /// How much of the sun's quad the eye can see, as the sun glare fader wants it: the two counts
    /// the eye's rays take, zeroed before the trace and read after it, and the share eased toward
    /// what they say. `shaders/glare.h` says whose query this stands for; `tone.comp` lays the
    /// wash over the picture by it.
    class SunGlarePass
    {
    public:
        SunGlarePass(const Device& device, const std::filesystem::path& shaderDirectory);

        /// Zeroes the counts, ahead of the trace that adds to them.
        void begin(VkCommandBuffer commands) const;

        /// Eases the share toward what the trace counted, where `getShare` points.
        ///
        /// @param elapsedSeconds since the previous frame.
        /// @param reset true where there is no previous share to move from — the first frame, and
        ///        any frame the renderer was told has no past. What the rays found is taken outright.
        void record(VkCommandBuffer commands, float elapsedSeconds, bool reset) const;

        /// The two counts, which the trace is bound at `BIND_SUN_GLARE`.
        const Buffer& getCounts() const { return mCounts; }

        /// One float, nought to one, written by `record`.
        const Buffer& getShare() const { return mShare; }

        /// One float holding nought, which is what a picture inside the interface is mapped with:
        /// it has no sun to be glared by, and the frame's share is the frame's.
        const Buffer& getNoShare() const { return mNoShare; }

    private:
        ComputePipeline mPipeline;

        Buffer mCounts;
        Buffer mShare;

        /// Host memory written once and never again.
        Buffer mNoShare;
    };
}
